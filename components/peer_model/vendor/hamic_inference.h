#ifndef HAMIC_INFERENCE_H
#define HAMIC_INFERENCE_H
#include "hamic_frontend.h"
#include "hamic_model.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    /* Each row is one second. env: temperature, humidity, CO2, RAW lux, dB. */
    float env[10][5];
    /* static, micro, slow, fast counts, event code (0..3). */
    float radar[10][5];
    double epoch_seconds; /* Unix time of FIRST row, including fractional seconds. */
    int32_t zone, room;   /* bed1=0/bedroom=0, desk=3/work=3. */
} HamicSensorInput;
typedef struct {
    float logits[HAMIC_CLASS_COUNT], probabilities[HAMIC_CLASS_COUNT];
    int32_t predicted_class;
} HamicOutput;
/* Allocate >= this many bytes aligned to16; one private workspace per task.
 * No allocations occur during inference. PCM and workspace must not overlap. */
size_t hamic_workspace_bytes(void);
int hamic_predict_pcm(const float *pcm, size_t sample_count,
                      const HamicSensorInput *sensor, void *workspace,
                      size_t workspace_bytes, HamicOutput *output);
/* Diagnostic/integration APIs. Audio input already contains normalized log-Mel.
 * Sensor input always contains RAW physical values. Feature arrays must not
 * overlap the workspace. These functions use the same trained weights. */
int hamic_encode_audio(const float *normalized_mel, void *workspace, size_t bytes, float audio96[96]);
int hamic_encode_sensor(const HamicSensorInput *input, void *workspace, size_t bytes, float sensor32[32]);
int hamic_fuse_features(const float audio96[96], const float sensor32[32], HamicOutput *output);
#ifdef __cplusplus
}
#endif
#endif
