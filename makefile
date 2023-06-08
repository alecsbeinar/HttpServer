CC = gcc
CFLAGS = -std=c11 -pedantic -Wall -Wextra -Werror -Wno-unused-parameter -D_DEFAULT_SOURCE

MAIN = main.c 

.PHONY: run
run:
	make server
.PHONY: clean


server: $(MAIN)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -rf server
