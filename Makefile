CC ?= cc
DB_BACKEND ?= sqlite
CPPFLAGS += -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
SOURCES = src/server.c src/workspace.c src/accounts.c src/storage.c src/procurement.c src/documents.c src/organization.c src/alerts.c src/mail_client.c src/calendar_client.c src/work_board.c src/inventory_client.c
SOURCES += auth/oidc.c src/ticket_bridge.c src/access_control.c
SOURCES += src/improvements.c src/improvements_governance.c src/improvements_operations.c src/improvements_integrations.c
CPPFLAGS += $(shell pkg-config --cflags openssl)
LDLIBS += $(shell pkg-config --libs openssl)
CPPFLAGS += $(shell pkg-config --cflags json-c)
LDLIBS += -lcurl $(shell pkg-config --libs json-c)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
LDLIBS += -framework Security
else
LDLIBS += -lcrypto
endif
ifeq ($(DB_BACKEND),mysql)
CPPFLAGS += -DPUNTO_MYSQL $(shell pkg-config --cflags libmariadb libmongoc-1.0)
LDLIBS += $(shell pkg-config --libs libmariadb libmongoc-1.0)
SOURCES += src/db_mysql.c src/mysql_schema.c
else ifeq ($(DB_BACKEND),sqlite)
LDLIBS += -lsqlite3
else
$(error DB_BACKEND deve essere sqlite oppure mysql)
endif

.PHONY: all run test clean FORCE
all: build/punto

build/$(DB_BACKEND)/punto: $(SOURCES) $(wildcard src/*.h src/*.inc auth/*.h) Makefile
	mkdir -p build/$(DB_BACKEND)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) -o $@ $(LDLIBS)

build/punto: build/$(DB_BACKEND)/punto FORCE
	cp build/$(DB_BACKEND)/punto $@.tmp
	mv -f $@.tmp $@

run: build/punto
	./build/punto

test: build/punto
	python3 -m unittest discover -s tests -v

clean:
	rm -rf build

.PHONY: tickets run-tickets test-tickets
tickets:
	$(MAKE) -C ticket-app

run-tickets: tickets
	cd ticket-app && ./build/punto-tickets

test-tickets: tickets build/punto
	python3 -m unittest discover -s tests -p 'test_ticket*.py' -v
