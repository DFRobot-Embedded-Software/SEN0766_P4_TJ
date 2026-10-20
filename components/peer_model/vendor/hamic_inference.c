#include "hamic_inference.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    float features[370], x[320], y[320], depthwise[320], concatenated[1280];
} SensorWorkspace;
typedef struct {
    float mel[HAMIC_FEATURES];
    float large[2][HAMIC_AUDIO_LARGE_FLOATS];
    float branch[3][HAMIC_AUDIO_BRANCH_FLOATS];
    SensorWorkspace sensor;
    HamicFrontendWorkspace frontend;
} InferenceWorkspace;

size_t hamic_workspace_bytes(void) { return (sizeof(InferenceWorkspace)+15u)&~(size_t)15u; }
static InferenceWorkspace *workspace_get(void *p, size_t size)
{
    if (!p || size < hamic_workspace_bytes() || ((uintptr_t)p & 15u)) return NULL;
    return (InferenceWorkspace *)p;
}
static int finite_array(const float *p, size_t n)
{
    if (!p) return 0;
    for (size_t i=0;i<n;++i) if (!isfinite(p[i])) return 0;
    return 1;
}
static void dense(const float *input, float *output, const HamicDense *layer)
{
    for (int o=0;o<layer->outputs;++o) {
        float sum=layer->bias ? layer->bias[o] : 0.0f;
        for (int i=0;i<layer->inputs;++i) sum+=input[i]*layer->weight[o*layer->inputs+i];
        output[o]=sum;
    }
}
static float bn(float value, int c, const HamicBN *layer)
{
    return (value-layer->mean[c])/sqrtf(layer->variance[c]+layer->epsilon)
           *layer->weight[c]+layer->bias[c];
}
static float gelu(float value)
{
    return 0.5f*value*(1.0f+erff(value*0.7071067811865475f));
}
static void layer_norm(const float *input, float *output, const HamicLayerNorm *layer)
{
    float mean=0.0f,variance=0.0f;
    for (int c=0;c<layer->channels;++c) mean+=input[c];
    mean/=layer->channels;
    for (int c=0;c<layer->channels;++c) { float d=input[c]-mean;variance+=d*d; }
    float inverse=1.0f/sqrtf(variance/layer->channels+layer->epsilon);
    for (int c=0;c<layer->channels;++c)
        output[c]=(input[c]-mean)*inverse*layer->weight[c]+layer->bias[c];
}
/* Single-sample CHW convolution. Audio BatchNorm is folded into weight/bias. */
static int conv2d(const float *input, int height, int width, const HamicConv2D *layer,
                  float *output, size_t capacity, int *out_h, int *out_w)
{
    int oh=(height+2*layer->ph-layer->kh)/layer->sh+1;
    int ow=(width+2*layer->pw-layer->kw)/layer->sw+1;
    if (oh<=0 || ow<=0 || (size_t)oh*ow*layer->out_channels>capacity) return HAMIC_WORKSPACE_ERROR;
    const int in_plane=height*width, out_plane=oh*ow;
    if (layer->kh==1 && layer->kw==1 && layer->sh==1 && layer->sw==1 && layer->groups==1) {
        for (int o=0;o<layer->out_channels;++o) {
            float *dest=output+o*out_plane;
            memset(dest,0,(size_t)out_plane*sizeof(float));
            for (int c=0;c<layer->in_channels;++c) {
                float weight=layer->weight[o*layer->in_channels+c];
                const float *src=input+c*in_plane;
                for (int p=0;p<out_plane;++p) dest[p]+=src[p]*weight;
            }
        }
    } else {
        int inputs_per_group=layer->in_channels/layer->groups;
        int outputs_per_group=layer->out_channels/layer->groups;
        for (int o=0;o<layer->out_channels;++o) {
            int first_channel=(o/outputs_per_group)*inputs_per_group;
            for (int y=0;y<oh;++y) for (int x=0;x<ow;++x) {
                float sum=0.0f;
                for (int c=0;c<inputs_per_group;++c)
                    for (int ky=0;ky<layer->kh;++ky) {
                        int iy=y*layer->sh+ky-layer->ph;
                        if (iy<0 || iy>=height) continue;
                        for (int kx=0;kx<layer->kw;++kx) {
                            int ix=x*layer->sw+kx-layer->pw;
                            if (ix<0 || ix>=width) continue;
                            int wi=((o*inputs_per_group+c)*layer->kh+ky)*layer->kw+kx;
                            sum+=input[(first_channel+c)*in_plane+iy*width+ix]*layer->weight[wi];
                        }
                    }
                output[o*out_plane+y*ow+x]=sum;
            }
        }
    }
    for (int o=0;o<layer->out_channels;++o) {
        float bias=layer->bias ? layer->bias[o] : 0.0f;
        for (int p=0;p<out_plane;++p) {
            float v=output[o*out_plane+p]+bias;
            if (layer->relu6) v=fminf(6.0f,fmaxf(0.0f,v));
            output[o*out_plane+p]=v;
        }
    }
    *out_h=oh;*out_w=ow;
    return HAMIC_OK;
}
#define CHECK(call) do { int rc=(call); if (rc!=HAMIC_OK) return rc; } while (0)
int hamic_encode_audio(const float *mel, void *workspace, size_t bytes, float audio96[96])
{
    InferenceWorkspace *w=workspace_get(workspace,bytes);
    if (!w) return HAMIC_WORKSPACE_ERROR;
    if (!mel || !audio96) return HAMIC_INVALID_ARGUMENT;
    if (!finite_array(mel,HAMIC_FEATURES)) return HAMIC_NONFINITE;
    float *current=w->large[0];int height,width;
    CHECK(conv2d(mel,HAMIC_FRAMES,HAMIC_MELS,&hamic_model.stem,current,
                  HAMIC_AUDIO_LARGE_FLOATS,&height,&width));
    for (int b=0;b<5;++b) {
        const HamicAudioBlock *block=&hamic_model.audio_blocks[b];
        float *down=current==w->large[0] ? w->large[1] : w->large[0];
        float *short_path=w->branch[0], *long_path=w->branch[1], *temp=w->branch[2];
        int dh,dw,ph,pw;
        CHECK(conv2d(current,height,width,&block->downsample,down,HAMIC_AUDIO_LARGE_FLOATS,&dh,&dw));
        CHECK(conv2d(down,dh,dw,&block->projection[0],short_path,HAMIC_AUDIO_BRANCH_FLOATS,&ph,&pw));
        if (ph!=dh || pw!=dw) return HAMIC_INVALID_ARGUMENT;
        CHECK(conv2d(down,dh,dw,&block->projection[1],long_path,HAMIC_AUDIO_BRANCH_FLOATS,&ph,&pw));
        CHECK(conv2d(long_path,dh,dw,&block->depthwise,temp,HAMIC_AUDIO_BRANCH_FLOATS,&ph,&pw));
        CHECK(conv2d(temp,dh,dw,&block->pointwise,long_path,HAMIC_AUDIO_BRANCH_FLOATS,&ph,&pw));
        size_t branch_values=(size_t)dh*dw*block->projection[0].out_channels;
        if (2*branch_values>HAMIC_AUDIO_LARGE_FLOATS) return HAMIC_WORKSPACE_ERROR;
        memcpy(current,short_path,branch_values*sizeof(float));
        memcpy(current+branch_values,long_path,branch_values*sizeof(float));
        CHECK(conv2d(current,dh,dw,&block->fusion,down,HAMIC_AUDIO_LARGE_FLOATS,&height,&width));
        current=down;
    }
    for (int c=0;c<96;++c) {
        float sum=0.0f;
        for (int p=0;p<height*width;++p) sum+=current[c*height*width+p];
        audio96[c]=sum/(height*width);
    }
    return finite_array(audio96,96) ? HAMIC_OK : HAMIC_NONFINITE;
}
static void temporal_block(const float *input, float *output, const HamicMS *block, SensorWorkspace *w)
{
    for (int t=0;t<10;++t) memcpy(w->concatenated+t*128,input+t*32,32*sizeof(float));
    for (int b=0;b<3;++b) {
        const HamicDS *branch=&block->branches[b];
        for (int t=0;t<10;++t) for (int c=0;c<32;++c) {
            float sum=0.0f;
            for (int k=0;k<branch->kernel;++k) {
                int source=t+k-branch->kernel/2;
                if (source>=0 && source<10) sum+=input[source*32+c]*branch->depthwise[c*branch->kernel+k];
            }
            w->depthwise[t*32+c]=gelu(bn(sum,c,&branch->depthwise_bn));
        }
        for (int t=0;t<10;++t) {
            float *dest=w->concatenated+t*128+(b+1)*32;
            dense(w->depthwise+t*32,dest,&branch->pointwise);
            for (int c=0;c<32;++c) dest[c]=gelu(bn(dest[c],c,&branch->pointwise_bn));
        }
    }
    for (int t=0;t<10;++t) {
        dense(w->concatenated+t*128,output+t*32,&block->fusion);
        for (int c=0;c<32;++c) {
            float residual=input[t*32+c]+bn(output[t*32+c],c,&block->fusion_bn);
            output[t*32+c]=fmaxf(0.0f,bn(residual,c,&block->output_bn));
        }
    }
}
int hamic_encode_sensor(const HamicSensorInput *input, void *workspace, size_t bytes, float sensor32[32])
{
    InferenceWorkspace *work=workspace_get(workspace,bytes);
    if (!work) return HAMIC_WORKSPACE_ERROR;
    if (!input || !sensor32 || !isfinite(input->epoch_seconds) || input->zone<0 || input->zone>=7 || input->room<0 || input->room>=7)
        return HAMIC_INVALID_ARGUMENT;
    SensorWorkspace *w=&work->sensor;
    const HamicModel *m=&hamic_model;
    for (int t=0;t<10;++t) {
        float *f=w->features+t*37;
        for (int c=0;c<5;++c) {
            float v=input->env[t][c];
            if (!isfinite(v) || (c==3 && v<0)) return HAMIC_INVALID_ARGUMENT;
            if (c==3) v=log1pf(v);
            f[c]=(v-m->env_mean[c])/(m->env_std[c]+1e-8f);
        }
        float ev=input->radar[t][4];
        if (!isfinite(ev) || ev<0 || ev>3 || ev!=floorf(ev)) return HAMIC_INVALID_ARGUMENT;
        float counts[4],region[20];
        for (int c=0;c<4;++c) {
            float v=input->radar[t][c];
            if (!isfinite(v) || v<0) return HAMIC_INVALID_ARGUMENT;
            counts[c]=(v-m->count_mean[c])/(m->count_std[c]+1e-8f);
        }
        memcpy(region,m->zone_embedding+input->zone*8,8*sizeof(float));
        dense(counts,region+8,&m->count_projection);
        memcpy(region+16,m->event_embedding+(int)ev*4,4*sizeof(float));
        dense(region,f+5,&m->region_projection);
        float day=(float)((input->epoch_seconds+t-m->time_reference)/86400.0);
        float time=(day-m->time_mean)/(m->time_std+1e-8f);
        dense(&time,f+21,&m->time_linear);
        dense(&time,f+22,&m->time_periodic);
        for (int c=22;c<29;++c) f[c]=sinf(f[c]);
        memcpy(f+29,m->room_embedding+input->room*8,8*sizeof(float));
        dense(f,w->x+t*32,&m->input_projection);
        for (int c=0;c<32;++c) w->x[t*32+c]=gelu(bn(w->x[t*32+c],c,&m->input_bn));
    }
    temporal_block(w->x,w->y,&m->sensor_blocks[0],w);
    temporal_block(w->y,w->x,&m->sensor_blocks[1],w);
    for (int c=0;c<32;++c) {
        float sum=0.0f;
        for (int t=0;t<10;++t) sum+=w->x[t*32+c];
        sensor32[c]=sum/10.0f;
    }
    return finite_array(sensor32,32) ? HAMIC_OK : HAMIC_NONFINITE;
}
int hamic_fuse_features(const float audio96[96], const float sensor32[32], HamicOutput *output)
{
    if (!audio96 || !sensor32 || !output) return HAMIC_INVALID_ARGUMENT;
    output->predicted_class=-1;
    if (!finite_array(audio96,96) || !finite_array(sensor32,32)) return HAMIC_NONFINITE;
    float projected[48],joined[80],hidden[32];
    dense(audio96,projected,&hamic_model.audio_projection);
    layer_norm(projected,joined,&hamic_model.audio_norm);
    layer_norm(sensor32,joined+48,&hamic_model.sensor_norm);
    dense(joined,hidden,&hamic_model.mlp_hidden);
    for (int i=0;i<32;++i) hidden[i]=fmaxf(0.0f,hidden[i]);
    dense(hidden,output->logits,&hamic_model.mlp_output);
    if (!finite_array(output->logits,HAMIC_CLASS_COUNT)) return HAMIC_NONFINITE;
    int best=0;
    for (int i=1;i<HAMIC_CLASS_COUNT;++i) if (output->logits[i]>output->logits[best]) best=i;
    float sum=0.0f;
    for (int i=0;i<HAMIC_CLASS_COUNT;++i) { output->probabilities[i]=expf(output->logits[i]-output->logits[best]);sum+=output->probabilities[i]; }
    for (int i=0;i<HAMIC_CLASS_COUNT;++i) output->probabilities[i]/=sum;
    output->predicted_class=best;
    return HAMIC_OK;
}
int hamic_predict_pcm(const float *pcm, size_t count, const HamicSensorInput *sensor,
                      void *workspace, size_t bytes, HamicOutput *output)
{
    InferenceWorkspace *w=workspace_get(workspace,bytes);
    if (!w) return HAMIC_WORKSPACE_ERROR;
    if (!output || !sensor || !pcm || count!=HAMIC_SAMPLES) return HAMIC_INVALID_ARGUMENT;
    output->predicted_class=-1;
    float audio96[96],sensor32[32];
    CHECK(hamic_frontend_16k(pcm,count,w->mel,&w->frontend,NULL));
    CHECK(hamic_encode_audio(w->mel,workspace,bytes,audio96));
    CHECK(hamic_encode_sensor(sensor,workspace,bytes,sensor32));
    return hamic_fuse_features(audio96,sensor32,output);
}
