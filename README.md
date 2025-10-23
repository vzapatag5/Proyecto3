# GSEA (RLE + XOR)

Proyecto en C que:
- Lee/escribe con syscalls (open/read/write/close)
- Comprime con RLE
- Encripta con XOR
- Pipeline: -c/-d/-e/-u

## Build
make

## Uso
./gsea -c -e -i tests/original.txt -o out/comp_enc.bin -k "clave"
./gsea -u -d -i out/comp_enc.bin -o tests/back.txt -k "clave"

## Requisitos
- WSL2 + Ubuntu
- build-essential, make, gdb, git
