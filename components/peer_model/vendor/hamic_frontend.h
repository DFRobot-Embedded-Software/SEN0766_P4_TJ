#ifndef HAMIC_FRONTEND_H
#define HAMIC_FRONTEND_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define HAMIC_SAMPLE_RATE 16000
#define HAMIC_SAMPLES 160000
#define HAMIC_FRAME_LENGTH 400
#define HAMIC_HOP_LENGTH 400
#define HAMIC_NFFT 512
#define HAMIC_FRAMES 400
#define HAMIC_MELS 64
#define HAMIC_FEATURES (HAMIC_FRAMES * HAMIC_MELS)
enum { HAMIC_OK=0, HAMIC_INVALID_ARGUMENT=-1, HAMIC_NONFINITE=-2,
       HAMIC_FFT_ERROR=-3, HAMIC_WORKSPACE_ERROR=-4 };
typedef struct { float fft[HAMIC_NFFT*2], power[HAMIC_NFFT/2+1]; } HamicFrontendWorkspace;
typedef int (*HamicFFT512)(float *complex_interleaved);
int hamic_fft512_portable(float *data);
/* Input: 10 seconds of scaled mono 16kHz float PCM. Output: normalized [400,64]. */
int hamic_frontend_16k(const float *audio, size_t sample_count, float *output,
                      HamicFrontendWorkspace *workspace, HamicFFT512 fft);
void hamic_pcm16_to_float(const int16_t *pcm, float *output, size_t count);
#ifdef __cplusplus
}
#endif
#endif
