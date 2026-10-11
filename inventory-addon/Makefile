CC ?= cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L $(shell pkg-config --cflags json-c openssl sqlite3)
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDLIBS += $(shell pkg-config --libs json-c openssl sqlite3)
.PHONY: all clean
all: build/punto-inventory
build/punto-inventory: src/inventory.c src/inventory_csv.inc src/inventory_due.inc src/inventory_custody.inc src/inventory_operations.inc src/inventory_directory.inc src/inventory_subjects.inc src/inventory_backup.c src/inventory_backup.h Makefile
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/inventory.c src/inventory_backup.c $(LDFLAGS) -o $@ $(LDLIBS)
clean:
	rm -rf build
