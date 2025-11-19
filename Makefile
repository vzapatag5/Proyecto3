CC=gcc
CFLAGS=-O0 -g -fsanitize=address -fno-omit-frame-pointer -Wall -Wextra -std=c11 -Iinclude -pthread -D_POSIX_C_SOURCE=200809L

# Añadir (si pkg-config está disponible):
OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
OPENSSL_LIBS   := $(shell pkg-config --libs   openssl 2>/dev/null)

# Si no hay OpenSSL, deshabilitar AES simple (stubs) para que compile igual
ifneq ($(strip $(OPENSSL_LIBS)),)
CFLAGS += $(OPENSSL_CFLAGS)
LDLIBS_OPENSSL := $(OPENSSL_LIBS)
else
CFLAGS += -DNO_OPENSSL
LDLIBS_OPENSSL :=
endif

OBJ = src/main.o src/rle_var.o src/lzw.o src/image_png.o src/image_jpeg.o src/fs.o src/vigenere.o src/huffman_predictor.o src/aes_simple.o src/audio_wav.o src/thread_pool.o src/journal.o
BIN=gsea

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $(BIN) $(CFLAGS) -lpng -ljpeg $(LDLIBS_OPENSSL)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)
