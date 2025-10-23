CC=gcc
CFLAGS=-O2 -Wall -Wextra -std=c11
LDFLAGS=

SRC=src/main.c src/fs.c src/rle.c src/xor.c
OBJ=$(SRC:.c=.o)

gsea: $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

clean:
	rm -f $(OBJ) gsea
