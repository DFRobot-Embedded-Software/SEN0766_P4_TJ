#include "hamic_frontend.h"
#include "hamic_model.h"

#include <math.h>
#include <string.h>

int hamic_fft512_portable(float *data)
{
    if (!data) return HAMIC_INVALID_ARGUMENT;
    /* Bit-reverse the input before the iterative radix-2 butterflies. */
    for (unsigned i = 1, j = 0; i < HAMIC_NFFT; ++i) {
        unsigned bit = HAMIC_NFFT >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float re = data[2 * i], im = data[2 * i + 1];
            data[2 * i] = data[2 * j];
            data[2 * i + 1] = data[2 * j + 1];
            data[2 * j] = re;
            data[2 * j + 1] = im;
        }
    }
    for (unsigned length = 2; length <= HAMIC_NFFT; length <<= 1) {
        unsigned half = length >> 1;
        unsigned step = HAMIC_NFFT / length;
        for (unsigned base = 0; base < HAMIC_NFFT; base += length) {
            for (unsigned j = 0; j < half; ++j) {
                unsigned a = 2 * (base + j), b = 2 * (base + j + half);
                float wr = hamic_twiddles[2 * j * step];
                float wi = hamic_twiddles[2 * j * step + 1];
                float tr = wr * data[b] - wi * data[b + 1];
                float ti = wr * data[b + 1] + wi * data[b];
                float ar = data[a], ai = data[a + 1];
                data[a] = ar + tr;
                data[a + 1] = ai + ti;
                data[b] = ar - tr;
                data[b + 1] = ai - ti;
            }
        }
    }
    return HAMIC_OK;
}

int hamic_frontend_16k(const float *audio, size_t sample_count,
                       float *output, HamicFrontendWorkspace *workspace, HamicFFT512 fft)
{
    if (!audio || !output || !workspace || sample_count != HAMIC_SAMPLES)
        return HAMIC_INVALID_ARGUMENT;
    for (size_t n = 0; n < sample_count; ++n)
        if (!isfinite(audio[n])) return HAMIC_NONFINITE;
    if (!fft) fft = hamic_fft512_portable;

    for (unsigned t = 0; t < HAMIC_FRAMES; ++t) {
        memset(workspace->fft, 0, sizeof(workspace->fft));
        for (unsigned n = 0; n < HAMIC_FRAME_LENGTH; ++n)
            workspace->fft[2 * n] = audio[t * HAMIC_HOP_LENGTH + n] * hamic_hamming[n];
        if (fft(workspace->fft) != 0) return HAMIC_FFT_ERROR;
        for (unsigned k = 0; k <= HAMIC_NFFT / 2; ++k) {
            float re = workspace->fft[2 * k], im = workspace->fft[2 * k + 1];
            workspace->power[k] = re * re + im * im;
            if (!isfinite(workspace->power[k])) return HAMIC_NONFINITE;
        }
        for (unsigned m = 0; m < HAMIC_MELS; ++m) {
            float energy = 0.0f;
            unsigned begin = hamic_mel_offset[m], end = hamic_mel_offset[m + 1];
            unsigned bin = hamic_mel_start[m];
            for (unsigned p = begin; p < end; ++p, ++bin)
                energy += workspace->power[bin] * hamic_mel_weight[p];
            float log_mel = logf(fmaxf(energy, 1.0e-10f));
            output[t * HAMIC_MELS + m] = (log_mel - hamic_mean[m]) / hamic_std[m];
        }
    }
    return HAMIC_OK;
}

void hamic_pcm16_to_float(const int16_t *pcm, float *output, size_t count)
{
    for (size_t n = 0; n < count; ++n) output[n] = (float)pcm[n] * (1.0f / 32768.0f);
}

