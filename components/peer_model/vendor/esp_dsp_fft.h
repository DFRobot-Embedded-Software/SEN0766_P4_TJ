#ifndef HAMIC_ESP_DSP_FFT_H
#define HAMIC_ESP_DSP_FFT_H
#ifdef __cplusplus
extern "C" {
#endif

/* ESP-IDF + ESP-DSP only. Initialize once before starting audio workers.
 * These functions return ESP_OK (0) or an ESP-DSP error code.
 * Share FFT table lifetime with other ESP-DSP users in the application. */
int hamic_esp_dsp_fft_init(void);
int hamic_esp_dsp_fft512(float *data);

#ifdef __cplusplus
}
#endif
#endif
