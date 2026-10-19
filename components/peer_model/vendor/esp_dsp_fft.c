/* Optional P4 adapter. Not part of the host build; board validation required. */
#include "esp_dsp_fft.h"
#include "hamic_frontend.h"
#include "esp_dsp.h"

int hamic_esp_dsp_fft_init(void)
{
    return dsps_fft2r_init_fc32(NULL, HAMIC_NFFT);
}

int hamic_esp_dsp_fft512(float *data)
{
    int result = dsps_fft2r_fc32(data, HAMIC_NFFT);
    if (result != ESP_OK) return result;
    /* Input is a full complex FFT with Im=0. Keep all 512 natural-order bins;
     * no packed-two-real-signal postprocessing and no FFT scaling. */
    return dsps_bit_rev_fc32(data, HAMIC_NFFT);
}
