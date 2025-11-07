CC=gcc
CFLAGS=-O0 -g -fsanitize=address -fno-omit-frame-pointer -Wall -Wextra -std=c11 -Iinclude -pthread -D_POSIX_C_SOURCE=200809L
OBJ=src/main.o src/fs.o src/rle_var.o src/vigenere.o src/image_png.o src/image_jpeg.o src/lzw.o
BIN=gsea

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $(BIN) $(CFLAGS) -lpng -ljpeg

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)
