CC=gcc
CFLAGS=-O0 -g -fsanitize=address -fno-omit-frame-pointer -Wall -Wextra -std=c11 -Iinclude -pthread
CFLAGS=-O0 -g -fsanitize=address -fno-omit-frame-pointer -Wall -Wextra -std=c11 -Iinclude -pthread -D_POSIX_C_SOURCE=200809L
OBJ=src/main.o src/fs.o src/rle_var.o src/vigenere.o
BIN=gsea

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $(BIN) $(CFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)
