#ifndef HAMIC_MODEL_H
#define HAMIC_MODEL_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define HAMIC_CLASS_COUNT 3
#define HAMIC_AUDIO_LARGE_FLOATS 102400
#define HAMIC_AUDIO_BRANCH_FLOATS 25600

typedef struct { int inputs, outputs; const float *weight, *bias; } HamicDense;
typedef struct { const float *weight, *bias, *mean, *variance; float epsilon; } HamicBN;
typedef struct { int channels; const float *weight, *bias; float epsilon; } HamicLayerNorm;
typedef struct {
    int in_channels, out_channels, kh, kw, sh, sw, ph, pw, groups, relu6;
    const float *weight, *bias;
} HamicConv2D;
typedef struct {
    HamicConv2D downsample, projection[2], depthwise, pointwise, fusion;
} HamicAudioBlock;
typedef struct {
    int kernel; const float *depthwise; HamicBN depthwise_bn;
    HamicDense pointwise; HamicBN pointwise_bn;
} HamicDS;
typedef struct { HamicDS branches[3]; HamicDense fusion; HamicBN fusion_bn, output_bn; } HamicMS;
typedef struct {
    HamicConv2D stem;
    HamicAudioBlock audio_blocks[5];
    const float *env_mean, *env_std, *count_mean, *count_std;
    float time_mean, time_std; double time_reference;
    const float *zone_embedding, *room_embedding, *event_embedding;
    HamicDense count_projection, region_projection, time_linear, time_periodic, input_projection;
    HamicBN input_bn;
    HamicMS sensor_blocks[2];
    HamicDense audio_projection, mlp_hidden, mlp_output;
    HamicLayerNorm audio_norm, sensor_norm;
} HamicModel;
extern const HamicModel hamic_model;
extern const char *const hamic_class_names[HAMIC_CLASS_COUNT];
extern const char hamic_checkpoint_sha256[];
extern const unsigned hamic_checkpoint_epoch;
extern const float hamic_hamming[400], hamic_twiddles[512];
extern const uint16_t hamic_mel_start[64], hamic_mel_offset[65];
extern const float hamic_mel_weight[];
extern const float hamic_mean[64], hamic_std[64];
#ifdef __cplusplus
}
#endif
#endif
