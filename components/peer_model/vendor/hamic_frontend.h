#ifndef HAMIC_FRONTEND_H
#define HAMIC_FRONTEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HAMIC_SAMPLE_RATE 16000
#define HAMIC_SAMPLES 160000
#define HAMIC_SOURCE_SAMPLES 441000
#define HAMIC_FRAME_LENGTH 400
#define HAMIC_HOP_LENGTH 400
#define HAMIC_NFFT 512
#define HAMIC_FRAMES 400
#define HAMIC_MELS 64
#define HAMIC_FEATURES (HAMIC_FRAMES * HAMIC_MELS)

enum { HAMIC_OK = 0, HAMIC_INVALID_ARGUMENT = -1,
       HAMIC_NONFINITE = -2, HAMIC_FFT_ERROR = -3 };

/* Allocate one workspace per concurrent frontend call. On P4, align it to
 * 16 bytes and prefer internal RAM. Output/input/workspace must not overlap. */
typedef struct {
    float fft[HAMIC_NFFT * 2];
    float power[HAMIC_NFFT / 2 + 1];
} HamicWorkspace;

/* In-place, unnormalized forward complex FFT, natural frequency order.
 * data = {Re[0], Im[0], ..., Re[511], Im[511]}. Return 0 on success. */
typedef int (*HamicFFT512)(float *data);
int hamic_fft512_portable(float *data);

/* Exactly 160000 already-scaled, mono float PCM samples at 16 kHz.
 * output needs HAMIC_FEATURES floats, contiguous [time][mel], representing
 * normalized model input [1,1,400,64]. Pass NULL for the portable FFT. */
int hamic_frontend_16k(const float *audio, size_t sample_count,
                       float *output, HamicWorkspace *workspace, HamicFFT512 fft);

/* Exactly one 10 s 44.1 kHz window -> 160000 samples at 16 kHz.
 * Same frozen FIR and independent zero boundaries as training resample_poly.
 * Caller owns output (160000 floats); no allocation and no cross-window state. */
int hamic_resample_44100(const float *audio, size_t sample_count, float *output);

/* Signed PCM conversion only, no peak/RMS normalization or clipping.
 * Caller supplies non-overlapping buffers, each with count elements.
 * Right-aligned PCM24 in int32 requires a different scale; see README. */
void hamic_pcm16_to_float(const int16_t *pcm, float *output, size_t count);
void hamic_pcm32_to_float(const int32_t *pcm, float *output, size_t count);

#ifdef __cplusplus
}
#endif
#endif
