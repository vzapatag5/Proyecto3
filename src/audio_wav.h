#ifndef AUDIO_WAV_H
#define AUDIO_WAV_H

#include <stdint.h>
#include <stddef.h>

/* Devuelve 1 si el buffer parece un WAV (RIFF/WAVE), 0 si no. */
int wav_is_riff_wave(const uint8_t* p, size_t n);

/* Decodifica WAV PCM16 LE.
 * Salidas:
 *  - out_samples: buffer malloc con nsamples * channels elementos int16_t
 *  - out_nsamples: número de frames (muestras por canal)
 *  - out_channels: 1=mono, 2=stereo, etc
 *  - out_samplerate: Hz
 * Retorna 0 ok, !=0 error.
 */
int wav_decode_pcm16(const uint8_t* in, size_t in_len,
                     int16_t** out_samples, size_t* out_nsamples,
                     int* out_channels, int* out_samplerate);

/* Codifica WAV PCM16 LE a partir de muestras intercaladas (frames * channels).
 * Retorna 0 ok, !=0 error.
 */
int wav_encode_pcm16(const int16_t* samples, size_t nsamples,
                     int channels, int samplerate,
                     uint8_t** out_wav, size_t* out_wav_len);

#endif
