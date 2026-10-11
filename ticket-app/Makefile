CC ?= cc
PKG_CONFIG ?= pkg-config
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -I../auth $(shell $(PKG_CONFIG) --cflags sqlite3 json-c openssl libcurl)
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDLIBS += $(shell $(PKG_CONFIG) --libs sqlite3 json-c openssl libcurl)

.PHONY: all clean
all: build/punto-tickets build/punto-ticket-receiver

build/punto-tickets: src/tickets.c src/punto_client.c src/punto_client.h src/punto_integration.inc src/ticket_calendar.inc $(wildcard src/raw*.h src/raw*.inc src/*operations*.h src/*operations*.inc src/*communications*.h src/*communications*.inc) ../src/mail_client.c ../src/mail_client.h ../auth/oidc.c ../auth/oidc.h ../auth/http_transport.h Makefile
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/tickets.c src/punto_client.c ../auth/oidc.c ../src/mail_client.c $(LDFLAGS) -o $@ $(LDLIBS)

build/punto-ticket-receiver: src/mail_receiver.c src/mail_mime.c src/mail_mime.h src/raw_store.c src/raw_store.h src/raw_schema.h Makefile
	mkdir -p build
	$(CC) $(CPPFLAGS) $(shell $(PKG_CONFIG) --cflags gmime-3.0) $(CFLAGS) src/mail_receiver.c src/mail_mime.c src/raw_store.c $(LDFLAGS) -o $@ $(LDLIBS) $(shell $(PKG_CONFIG) --libs gmime-3.0)

clean:
	rm -rf build
