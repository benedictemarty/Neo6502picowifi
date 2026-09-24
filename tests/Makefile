# Tests du cœur du modem sur PC : make -C tests test
# test_roots_ca_cb compile mbedTLS du SDK sur PC : PICO_SDK_PATH (avec
# lib/mbedtls initialisé) ou MBEDTLS_DIR ; sinon ce test est SAUTÉ (signalé).
CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined
PYTHON ?= python3
MBEDTLS_DIR ?= $(PICO_SDK_PATH)/lib/mbedtls
HAVE_MBEDTLS := $(wildcard $(MBEDTLS_DIR)/library/x509_crt.c)
MBED_CFLAGS = -std=c11 -g -O1 -fsanitize=address,undefined -I. -I$(MBEDTLS_DIR)/include \
	-DMBEDTLS_CONFIG_FILE='"mbedtls_host_config.h"'
MBED_SRCS = $(wildcard $(MBEDTLS_DIR)/library/*.c)
MBED_OBJS = $(patsubst $(MBEDTLS_DIR)/library/%.c,mbedtls-host/%.o,$(MBED_SRCS))

test: test_at_modem test_tls_date test_roots_store test_roots2c test_version test_roots_ca_cb_run
	./test_at_modem
	./test_tls_date
	./test_roots_store

test_roots2c:
	$(PYTHON) test_roots2c.py

test_version:
	$(PYTHON) test_version.py

test_tls_date: test_tls_date.c ../src/tls_date.c ../src/tls_date.h
	$(CC) $(CFLAGS) -o $@ test_tls_date.c ../src/tls_date.c

test_at_modem: test_at_modem.c ../src/at_modem.c ../src/at_modem.h
	$(CC) $(CFLAGS) -o $@ test_at_modem.c ../src/at_modem.c

roots_gen.c: ../certs/roots.pem ../tools/roots2c.py
	$(PYTHON) ../tools/roots2c.py ../certs/roots.pem $@

roots_fixture_gen.c: fixtures/store.pem ../tools/roots2c.py
	$(PYTHON) ../tools/roots2c.py fixtures/store.pem $@ --symbol roots_fixture 2>/dev/null

test_roots_store: test_roots_store.c ../src/roots_store.c ../src/roots_store.h roots_gen.c
	$(CC) $(CFLAGS) -I../src -o $@ test_roots_store.c ../src/roots_store.c roots_gen.c

ifneq ($(HAVE_MBEDTLS),)
test_roots_ca_cb_run: test_roots_ca_cb
	./test_roots_ca_cb

mbedtls-host/%.o: $(MBEDTLS_DIR)/library/%.c mbedtls_host_config.h
	@mkdir -p mbedtls-host
	@$(CC) $(MBED_CFLAGS) -I$(MBEDTLS_DIR)/library -c $< -o $@

mbedtls-host/libmbedhost.a: $(MBED_OBJS)
	ar rcs $@ $^

test_roots_ca_cb: test_roots_ca_cb.c ../src/roots_ca_cb.c ../src/roots_ca_cb.h ../src/roots_store.c \
		roots_gen.c roots_fixture_gen.c mbedtls-host/libmbedhost.a
	$(CC) $(CFLAGS) $(MBED_CFLAGS) -I../src -o $@ test_roots_ca_cb.c ../src/roots_ca_cb.c \
		../src/roots_store.c roots_gen.c roots_fixture_gen.c mbedtls-host/libmbedhost.a
else
test_roots_ca_cb_run:
	@echo "*** test_roots_ca_cb SAUTÉ : mbedTLS introuvable ($(MBEDTLS_DIR)) — définir PICO_SDK_PATH ou MBEDTLS_DIR ***"
endif

clean:
	rm -rf test_at_modem test_tls_date test_roots_store test_roots_ca_cb roots_gen.c roots_fixture_gen.c mbedtls-host

.PHONY: test test_roots2c test_version test_roots_ca_cb_run clean
