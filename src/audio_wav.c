/*
 * ====================================================================
 * Manejo de archivos WAV (audio sin compresión PCM 16-bit)
 * ====================================================================
 * 
 * Este módulo lee y escribe archivos de audio WAV en formato PCM 16-bit.
 * Se usa para procesar audio con Delta16 + LZW/Huffman para mejor compresión.
 */

#include "audio_wav.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========== Helpers para leer/escribir números en little-endian ========== */
/* WAV guarda números en formato little-endian (byte menos significativo primero) */

/* Leer un número de 32 bits (4 bytes) */
static uint32_t rd32le(const uint8_t* p){ 
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); 
}

/* Leer un número de 16 bits (2 bytes) */
static uint16_t rd16le(const uint8_t* p){ 
    return (uint16_t)p[0] | ((uint16_t)p[1]<<8); 
}

/* Escribir un número de 32 bits en 4 bytes */
static void wr32le(uint8_t* p, uint32_t v){ 
    p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; 
}

/* Escribir un número de 16 bits en 2 bytes */
static void wr16le(uint8_t* p, uint16_t v){ 
    p[0]=v&0xFF; p[1]=(v>>8)&0xFF; 
}

/*
 * Verifica si un buffer es un archivo WAV válido
 * 
 * Un WAV empieza con: 'RIFF' [tamaño] 'WAVE'
 * Esto está en los primeros 12 bytes.
 */
int wav_is_riff_wave(const uint8_t* p, size_t n) {
    if (!p || n < 12) return 0;
    
    /* Verificar firma: 'RIFF' en posición 0-3 y 'WAVE' en posición 8-11 */
    return (p[0]=='R' && p[1]=='I' && p[2]=='F' && p[3]=='F'
         && p[8]=='W' && p[9]=='A' && p[10]=='V' && p[11]=='E');
}

/*
 * Lee un archivo WAV y extrae las muestras de audio PCM 16-bit
 * 
 * Parámetros:
 * - in: buffer con el archivo WAV completo
 * - in_len: tamaño del buffer
 * - out_samples: muestras extraídas (int16, intercaladas si es estéreo)
 * - out_nsamples: número de "frames" (si estéreo, cada frame = 2 samples)
 * - out_channels: 1=mono, 2=estéreo
 * - out_samplerate: ej: 44100 Hz
 * 
 * Retorna: 0 si OK, negativo si falla
 */
int wav_decode_pcm16(const uint8_t* in, size_t in_len,
                     int16_t** out_samples, size_t* out_nsamples,
                     int* out_channels, int* out_samplerate)
{
    /* Verificar que sea un WAV válido */
    if (!wav_is_riff_wave(in, in_len)) return -1;

    /* Saltar el encabezado RIFF inicial (12 bytes: 'RIFF' + tamaño + 'WAVE') */
    size_t pos = 12;

    /* Variables para guardar información del formato */
    int fmt_found = 0;       /* ¿Encontramos el chunk 'fmt '? */
    int data_found = 0;      /* ¿Encontramos el chunk 'data'? */
    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint32_t samplerate = 0;
    uint16_t bits_per_sample = 0;
    const uint8_t* data_ptr = NULL;
    uint32_t data_size = 0;

    /* Recorrer todos los chunks del archivo WAV */
    while (pos + 8 <= in_len) {
        uint32_t cid = rd32le(in + pos); pos += 4; /* ID del chunk (4 bytes) */
        uint32_t csz = rd32le(in + pos); pos += 4; /* Tamaño del chunk */
        if (pos + csz > in_len) break;

        /* Chunk 'fmt ': contiene información del formato de audio */
        if (cid == 0x20746d66) { /* 'fmt ' en little-endian */
            if (csz < 16) return -2;
            audio_format    = rd16le(in + pos + 0);  /* 1 = PCM sin compresión */
            channels        = rd16le(in + pos + 2);  /* 1=mono, 2=estéreo */
            samplerate      = rd32le(in + pos + 4);  /* ej: 44100 Hz */
            bits_per_sample = rd16le(in + pos + 14); /* 16 bits por muestra */
            fmt_found = 1;
        } 
        /* Chunk 'data': contiene las muestras de audio */
        else if (cid == 0x61746164) { /* 'data' en little-endian */
            data_ptr = in + pos;
            data_size = csz;
            data_found = 1;
        }
        
        /* Saltar al siguiente chunk (algunos chunks tienen padding para alinear a par) */
        pos += csz + (csz & 1u);
    }

    /* Validar que encontramos ambos chunks */
    if (!fmt_found || !data_found) return -3;
    
    /* Solo aceptamos PCM sin compresión (formato 1) */
    if (audio_format != 1) return -4;
    
    /* Solo aceptamos 16 bits por muestra */
    if (bits_per_sample != 16) return -5;

    /* Calcular número de muestras (cada muestra = 2 bytes) */
    size_t total_samples = data_size / 2;
    if (total_samples == 0) return -6;
    
    /* Copiar las muestras al buffer de salida */
    int16_t* out = (int16_t*)malloc(data_size);
    if (!out) return -7;
    memcpy(out, data_ptr, data_size);

    /* Guardar resultados */
    *out_samples   = out;
    *out_nsamples  = total_samples / channels; /* frames (muestras por canal) */
    *out_channels  = channels;
    *out_samplerate= (int)samplerate;
    return 0;
}

/*
 * Crea un archivo WAV desde muestras PCM 16-bit
 * 
 * Parámetros:
 * - samples: muestras de audio (int16, intercaladas si es estéreo)
 * - nsamples: número de frames (si estéreo, cada frame = 2 samples)
 * - channels: 1=mono, 2=estéreo
 * - samplerate: ej: 44100 Hz
 * - out_wav: buffer de salida con el archivo WAV completo
 * - out_wav_len: tamaño del archivo WAV generado
 * 
 * Retorna: 0 si OK, negativo si falla
 */
int wav_encode_pcm16(const int16_t* samples, size_t nsamples,
                     int channels, int samplerate,
                     uint8_t** out_wav, size_t* out_wav_len)
{
    /* Validar entrada */
    if (!samples || nsamples==0 || channels<=0 || samplerate<=0) return -1;

    /* Calcular tamaños */
    uint32_t data_bytes = (uint32_t)(nsamples * channels * 2); /* bytes de audio */
    uint32_t fmt_chunk_size = 16;  /* tamaño estándar del chunk 'fmt ' */
    uint32_t riff_size = 4 /*'WAVE'*/ + (8+fmt_chunk_size) + (8+data_bytes);
    size_t total_len = 8 + riff_size;  /* 8 = 'RIFF' + tamaño */

    /* Alojar buffer para el archivo WAV completo */
    uint8_t* out = (uint8_t*)malloc(total_len);
    if (!out) return -2;

    /* ========== Escribir encabezado RIFF ========== */
    out[0]='R'; out[1]='I'; out[2]='F'; out[3]='F';
    wr32le(out+4, riff_size);  /* Tamaño del resto del archivo */
    out[8]='W'; out[9]='A'; out[10]='V'; out[11]='E';

    /* ========== Escribir chunk 'fmt ' (formato de audio) ========== */
    size_t p = 12;
    out[p+0]='f'; out[p+1]='m'; out[p+2]='t'; out[p+3]=' ';
    wr32le(out+p+4, fmt_chunk_size);   /* Tamaño del chunk fmt */
    wr16le(out+p+8, 1);                 /* Audio format: 1 = PCM */
    wr16le(out+p+10, (uint16_t)channels);     /* Número de canales */
    wr32le(out+p+12, (uint32_t)samplerate);   /* Sample rate (Hz) */
    
    /* Byte rate: cuántos bytes por segundo (samplerate × canales × 2 bytes) */
    uint32_t byte_rate = (uint32_t)(samplerate * channels * 2);
    wr32le(out+p+16, byte_rate);
    
    /* Block align: bytes por frame (canales × 2 bytes) */
    wr16le(out+p+20, (uint16_t)(channels*2));
    
    /* Bits por muestra */
    wr16le(out+p+22, 16);
    p += 8 + fmt_chunk_size;

    /* ========== Escribir chunk 'data' (muestras de audio) ========== */
    out[p+0]='d'; out[p+1]='a'; out[p+2]='t'; out[p+3]='a';
    wr32le(out+p+4, data_bytes);  /* Tamaño de los datos de audio */
    memcpy(out+p+8, samples, data_bytes);  /* Copiar las muestras */
    p += 8 + data_bytes;

    /* Guardar resultado */
    *out_wav = out;
    *out_wav_len = p;
    return 0;
}
