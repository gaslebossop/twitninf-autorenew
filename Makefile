# twitninf-autorenew
#
# Une seule dépendance : libpq. Pas de client HTTP, parce que le démon ne
# rappelle jamais le moteur anti-fraude — l'autorisation est prise une fois,
# à la signature du mandat, par l'API Node.
#
#   apt install build-essential libpq-dev
#   make && sudo make install

BIN      := twitninf-autorenew
PREFIX   ?= /usr/local
SRCDIR   := src
BUILDDIR := build

SOURCES  := $(wildcard $(SRCDIR)/*.c)
OBJECTS  := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SOURCES))
DEPS     := $(OBJECTS:.o=.d)

PG_INCLUDE := $(shell pg_config --includedir 2>/dev/null)
PG_LIBDIR  := $(shell pg_config --libdir 2>/dev/null)

CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS  += -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
CFLAGS  += -Wformat=2 -Wwrite-strings -Wstrict-prototypes
CFLAGS  += -MMD -MP
ifneq ($(PG_INCLUDE),)
CFLAGS  += -I$(PG_INCLUDE)
endif

LDFLAGS ?=
ifneq ($(PG_LIBDIR),)
LDFLAGS += -L$(PG_LIBDIR)
endif
LDLIBS  := -lpq

.PHONY: all clean install uninstall check

all: $(BIN)

$(BIN): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# Compilation seule, sans édition de liens : vérifie la syntaxe et les types
# sans exiger que libpq soit installée en version partagée.
check: $(BUILDDIR)
	@for f in $(SOURCES); do $(CC) $(CFLAGS) -fsyntax-only $$f || exit 1; done
	@echo "syntaxe et types : OK"

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -rf $(BUILDDIR) $(BIN)

-include $(DEPS)
