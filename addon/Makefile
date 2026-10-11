CC ?= cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
ifeq ($(shell uname -s),Darwin)
CPPFLAGS += -I/opt/homebrew/opt/openssl@3/include
LDFLAGS += -L/opt/homebrew/opt/openssl@3/lib
endif
LDLIBS += -lssl -lcrypto -lcurl -lsqlite3

.PHONY: all clean
all: build/punto-mailer

build/punto-mailer: src/mailer.c Makefile
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $@ $(LDLIBS)

clean:
	rm -rf build
