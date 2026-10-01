# Modem simulé sur PC (Linux) : make -C pc ; ./pc/pcmodem -l /tmp/neomodem [-T /tmp/neotnfs]
# TLS : mbedTLS du Pico SDK (PICO_SDK_PATH ou MBEDTLS_DIR) compilé avec la configuration
# du firmware ; sans lui, le modem est construit sans TLS (signalé).
CC ?= gcc
CFLAGS ?= -std=gnu11 -Wall -Wextra -Werror -O2 -g
PYTHON ?= python3
VERSION := $(shell cat ../VERSION)
MBEDTLS_DIR ?= $(PICO_SDK_PATH)/lib/mbedtls
HAVE_MBEDTLS := $(wildcard $(MBEDTLS_DIR)/library/ssl_tls.c)
SRCS = pcmodem.c ../src/at_modem.c ../src/http_parse.c ../src/tnfs_link.c ../src/tnfs_client.c
HDRS = ../src/at_modem.h ../src/http_parse.h ../src/tnfs_link.h ../src/tnfs_client.h ../VERSION

ifneq ($(HAVE_MBEDTLS),)
MBED_CFLAGS = -std=gnu11 -O2 -I. -I$(MBEDTLS_DIR)/include -DMBEDTLS_CONFIG_FILE='"mbedtls_pc_config.h"'
MBED_OBJS = $(patsubst $(MBEDTLS_DIR)/library/%.c,mbedtls-pc/%.o,$(wildcard $(MBEDTLS_DIR)/library/*.c))
TLS_SRCS = ../src/roots_store.c ../src/roots_ca_cb.c roots_gen.c

pcmodem: $(SRCS) $(HDRS) $(TLS_SRCS) mbedtls_pc_config.h ../src/mbedtls_config.h mbedtls-pc/libmbedpc.a
	$(CC) $(CFLAGS) $(MBED_CFLAGS) -I../src -DPCMODEM_VERSION='"$(VERSION)"' -o $@ $(SRCS) $(TLS_SRCS) mbedtls-pc/libmbedpc.a

mbedtls-pc/%.o: $(MBEDTLS_DIR)/library/%.c mbedtls_pc_config.h ../src/mbedtls_config.h
	@mkdir -p mbedtls-pc
	@$(CC) $(MBED_CFLAGS) -I$(MBEDTLS_DIR)/library -c $< -o $@

mbedtls-pc/libmbedpc.a: $(MBED_OBJS)
	ar rcs $@ $^

roots_gen.c: ../certs/roots.pem ../tools/roots2c.py
	$(PYTHON) ../tools/roots2c.py ../certs/roots.pem $@
else
pcmodem: $(SRCS) $(HDRS)
	@echo "*** pcmodem SANS TLS : mbedTLS introuvable ($(MBEDTLS_DIR)) — définir PICO_SDK_PATH ***"
	$(CC) $(CFLAGS) -DPCMODEM_NO_TLS -DPCMODEM_VERSION='"$(VERSION)"' -o $@ $(SRCS)
endif

clean:
	rm -rf pcmodem mbedtls-pc roots_gen.c

.PHONY: clean
