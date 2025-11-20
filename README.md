# GSEA

Herramienta educativa en C para compresión y cifrado de archivos y carpetas. Permite aplicar distintos algoritmos sobre archivos individuales o carpetas completas, con paralelismo y journaling opcional.

## Integrantes
- Valentina Zapata.  
- Mariana Carrasquilla.
- David García.

## Características
- Compresión: RLE, LZW, LZW+SUB (predictor), Huffman+Predictor interno, Delta16 (WAV) con LZW/Huffman.
- Cifrado: Vigenère (didáctico) y AES-256-CBC (si hay OpenSSL instalado).
- Paralelismo: externo (archivos en carpeta) e interno (chunks de archivos grandes).
- Journal opcional (`-j`) mostrando pasos, tamaños y tiempos.
- Modo interactivo (sin argumentos) para uso guiado.

## Archivos Clave
- Pipeline principal: `src/main.c`
- RLE: `src/rle_var.c`
- LZW: `src/lzw.c`
- Huffman + predictor: `src/huffman_predictor.c`
- WAV + delta16: `src/audio_wav.c`
- Cifrado: `src/vigenere.c`, `src/aes_simple.c`
- Hilos (pool): `src/thread_pool.c`
- Journal: `src/journal.c`
- FS (I/O): `src/fs.c`

## Compilación
Instalar dependencias (Ubuntu):
```bash
sudo apt install build-essential libpng-dev libjpeg-dev libssl-dev
```
Compilar:
```bash
make
```
Genera el ejecutable `gsea`.
Limpiar:
```bash
make clean
```
Si no hay OpenSSL se compila con `-DNO_OPENSSL` y AES queda deshabilitado.

## Uso Básico
Modo interactivo:
```bash
./gsea
```
Modo línea de comandos (estructura general):
```bash
./gsea [operaciones] [opciones] -i entrada -o salida
```
Operaciones combinables:
- `-c` comprimir
- `-d` descomprimir
- `-e` cifrar
- `-u` descifrar

Opciones principales:
- `--comp-alg rlevar|lzw|lzw-pred|huffman-pred|delta16-lzw|delta16-huff`
- `--enc-alg vigenere|aes|none`
- `-k <clave>` (requerida para AES/Vigenère)
- `--workers N|auto` hilos externos
- `--inner-workers N|auto` hilos internos para chunks
- `--chunk-mb <MB>` tamaño de chunk (default 100)
- `-j` activar journal
- `-i <ruta>` entrada / `-o <ruta>` salida

## Ejemplos
```bash
# Comprimir simple
./gsea -c --comp-alg lzw -i tests/archivo.txt -o out.bin

# Comprimir + cifrar AES
./gsea -c -e --comp-alg lzw --enc-alg aes -k miclave123 -i tests/archivo.txt -o out.bin

# Descomprimir + descifrar
./gsea -d -u --comp-alg lzw --enc-alg aes -k miclave123 -i out.bin -o recuperado.txt

# Carpeta con hilos automáticos
./gsea -c --comp-alg lzw --workers auto --inner-workers auto -i tests/ -o outdir/

# WAV delta16
./gsea -c --comp-alg delta16-lzw -i audio.wav -o audio.bin

# Journal activo
./gsea -c -j --comp-alg huffman-pred -i tests/archivo.txt -o out.bin
```

## Paralelismo
- Carpeta: cada archivo se procesa como tarea en el pool externo.
- Archivo grande: división en chunks y compresión paralela interna.

## Notas
- Huffman puede aumentar tamaño en datos ya comprimidos (PNG/JPEG).
- Vigenère es inseguro (solo educativo).
- Lectura/escritura se hace cargando el archivo completo (simplifica).
- Descompresión de chunks actualmente secuencial.
- AES requiere OpenSSL; si falta usar `--enc-alg vigenere` o `none`.

## Licencia
Uso académico / educativo.
