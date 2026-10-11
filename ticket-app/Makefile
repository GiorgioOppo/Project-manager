CC ?= cc
PKG_CONFIG ?= pkg-config
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -I../auth $(shell $(PKG_CONFIG) --cflags sqlite3 json-c openssl libcurl)
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDLIBS += $(shell $(PKG_CONFIG) --libs sqlite3 json-c openssl libcurl)

.PHONY: all clean
all: build/punto-tickets

build/punto-tickets: src/tickets.c src/punto_client.c src/punto_client.h src/punto_integration.inc src/ticket_calendar.inc ../auth/oidc.c ../auth/oidc.h ../auth/http_transport.h Makefile
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/tickets.c src/punto_client.c ../auth/oidc.c $(LDFLAGS) -o $@ $(LDLIBS)

clean:
	rm -rf build
