#include "hamic_frontend.h"
#include "hamic_tables.h"

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
                       float *output, HamicWorkspace *workspace, HamicFFT512 fft)
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

int hamic_resample_44100(const float *audio, size_t sample_count, float *output)
{
    if (!audio || !output || sample_count != HAMIC_SOURCE_SAMPLES)
        return HAMIC_INVALID_ARGUMENT;
    for (size_t n = 0; n < sample_count; ++n)
        if (!isfinite(audio[n])) return HAMIC_NONFINITE;
    /* U=160, D=441, half_len=4410. Delay-compensated convolution:
     * y[k] = sum_n x[n] h[441*k + 4410 - 160*n].
     * h includes the factor U, and out-of-window x is zero. */
    for (int k = 0; k < HAMIC_SAMPLES; ++k) {
        int center = 441 * k + 4410;
        int first = center > 8820 ? (center - 8820 + 159) / 160 : 0;
        int last = center / 160;
        if (last >= HAMIC_SOURCE_SAMPLES) last = HAMIC_SOURCE_SAMPLES - 1;
        float value = 0.0f;
        for (int n = first, tap = center - 160 * first; n <= last; ++n, tap -= 160)
            value += audio[n] * hamic_resample_fir[tap];
        if (!isfinite(value)) return HAMIC_NONFINITE;
        output[k] = value;
    }
    return HAMIC_OK;
}

void hamic_pcm16_to_float(const int16_t *pcm, float *output, size_t count)
{
    for (size_t n = 0; n < count; ++n) output[n] = (float)pcm[n] * (1.0f / 32768.0f);
}

void hamic_pcm32_to_float(const int32_t *pcm, float *output, size_t count)
{
    for (size_t n = 0; n < count; ++n) output[n] = (float)pcm[n] * (1.0f / 2147483648.0f);
}
