# Tests du cœur du modem sur PC : make -C tests test
CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined

test: test_at_modem
	./test_at_modem

test_at_modem: test_at_modem.c ../src/at_modem.c ../src/at_modem.h
	$(CC) $(CFLAGS) -o $@ test_at_modem.c ../src/at_modem.c

clean:
	rm -f test_at_modem

.PHONY: test clean
