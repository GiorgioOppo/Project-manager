CC ?= cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L $(shell pkg-config --cflags json-c openssl)
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDLIBS += $(shell pkg-config --libs json-c openssl) -lcurl -lsqlite3 -pthread
.PHONY: all clean
all: build/punto-calendar
build/punto-calendar: src/calendar.c Makefile
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $@ $(LDLIBS)
clean:
	rm -rf build
