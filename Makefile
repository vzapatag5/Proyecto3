CC=gcc
CFLAGS=-O0 -g -fsanitize=address -fno-omit-frame-pointer -Wall -Wextra -std=c11 -Iinclude -pthread -D_POSIX_C_SOURCE=200809L
OBJ = src/main.o src/rle_var.o src/lzw.o src/image_png.o src/image_jpeg.o src/fs.o src/vigenere.o src/huffman_predictor.o src/aes_simple.o
BIN=gsea

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $(BIN) $(CFLAGS) -lpng -ljpeg -lcrypto

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)
