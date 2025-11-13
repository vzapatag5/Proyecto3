#include "audio_wav.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static uint32_t rd32le(const uint8_t* p){ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint16_t rd16le(const uint8_t* p){ return (uint16_t)p[0] | ((uint16_t)p[1]<<8); }
static void     wr32le(uint8_t* p, uint32_t v){ p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }
static void     wr16le(uint8_t* p, uint16_t v){ p[0]=v&0xFF; p[1]=(v>>8)&0xFF; }

int wav_is_riff_wave(const uint8_t* p, size_t n) {
    if (!p || n < 12) return 0;
    /* 'RIFF' .... 'WAVE' */
    return (p[0]=='R' && p[1]=='I' && p[2]=='F' && p[3]=='F'
         && p[8]=='W' && p[9]=='A' && p[10]=='V' && p[11]=='E');
}

int wav_decode_pcm16(const uint8_t* in, size_t in_len,
                     int16_t** out_samples, size_t* out_nsamples,
                     int* out_channels, int* out_samplerate)
{
    if (!wav_is_riff_wave(in, in_len)) return -1;

    size_t pos = 12; /* saltar RIFF(4) + size(4) + WAVE(4) */

    int fmt_found = 0;
    int data_found = 0;
    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint32_t samplerate = 0;
    uint16_t bits_per_sample = 0;
    const uint8_t* data_ptr = NULL;
    uint32_t data_size = 0;

    while (pos + 8 <= in_len) {
        uint32_t cid = rd32le(in + pos); pos += 4; /* chunk id */
        uint32_t csz = rd32le(in + pos); pos += 4; /* chunk size */
        if (pos + csz > in_len) break;

        if (cid == 0x20746d66) { /* 'fmt ' */
            if (csz < 16) return -2;
            audio_format    = rd16le(in + pos + 0);
            channels        = rd16le(in + pos + 2);
            samplerate      = rd32le(in + pos + 4);
            bits_per_sample = rd16le(in + pos + 14);
            fmt_found = 1;
        } else if (cid == 0x61746164) { /* 'data' */
            data_ptr = in + pos;
            data_size = csz;
            data_found = 1;
        }
        pos += csz + (csz & 1u); /* los chunks WAV pueden tener padding a par */
    }

    if (!fmt_found || !data_found) return -3;
    if (audio_format != 1) return -4;            /* solo PCM */
    if (bits_per_sample != 16) return -5;        /* solo 16-bit */

    size_t total_samples = data_size / 2; /* int16 */
    if (total_samples == 0) return -6;
    int16_t* out = (int16_t*)malloc(data_size);
    if (!out) return -7;
    memcpy(out, data_ptr, data_size);

    *out_samples   = out;
    *out_nsamples  = total_samples / channels; /* frames */
    *out_channels  = channels;
    *out_samplerate= (int)samplerate;
    return 0;
}

int wav_encode_pcm16(const int16_t* samples, size_t nsamples,
                     int channels, int samplerate,
                     uint8_t** out_wav, size_t* out_wav_len)
{
    if (!samples || nsamples==0 || channels<=0 || samplerate<=0) return -1;

    uint32_t data_bytes = (uint32_t)(nsamples * channels * 2);
    uint32_t fmt_chunk_size = 16;
    uint32_t riff_size = 4 /*'WAVE'*/ + (8+fmt_chunk_size) + (8+data_bytes);
    size_t total_len = 8 + riff_size;

    uint8_t* out = (uint8_t*)malloc(total_len);
    if (!out) return -2;

    /* RIFF header */
    out[0]='R'; out[1]='I'; out[2]='F'; out[3]='F';
    wr32le(out+4, riff_size);
    out[8]='W'; out[9]='A'; out[10]='V'; out[11]='E';

    /* fmt chunk */
    size_t p = 12;
    out[p+0]='f'; out[p+1]='m'; out[p+2]='t'; out[p+3]=' ';
    wr32le(out+p+4, fmt_chunk_size);
    wr16le(out+p+8, 1); /* PCM */
    wr16le(out+p+10, (uint16_t)channels);
    wr32le(out+p+12, (uint32_t)samplerate);
    uint32_t byte_rate = (uint32_t)(samplerate * channels * 2);
    wr32le(out+p+16, byte_rate);
    wr16le(out+p+20, (uint16_t)(channels*2)); /* block align */
    wr16le(out+p+22, 16); /* bits per sample */
    p += 8 + fmt_chunk_size;

    /* data chunk */
    out[p+0]='d'; out[p+1]='a'; out[p+2]='t'; out[p+3]='a';
    wr32le(out+p+4, data_bytes);
    memcpy(out+p+8, samples, data_bytes);
    p += 8 + data_bytes;

    *out_wav = out;
    *out_wav_len = p;
    return 0;
}
