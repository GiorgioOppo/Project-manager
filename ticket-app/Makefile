CC ?= cc
PKG_CONFIG ?= pkg-config
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 $(shell $(PKG_CONFIG) --cflags sqlite3 json-c openssl)
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDLIBS += $(shell $(PKG_CONFIG) --libs sqlite3 json-c openssl)

.PHONY: all clean
all: build/punto-tickets

build/punto-tickets: src/tickets.c Makefile
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $@ $(LDLIBS)

clean:
	rm -rf build
