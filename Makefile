PREFIX ?= /usr/local
DESTDIR ?=
CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -Os
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags x11)
CFLAGS += -std=c99 -pedantic -Wall -Wextra
LDLIBS += $(shell $(PKG_CONFIG) --libs x11)

all: tinywm

tinywm: tinywm.c config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) tinywm.c $(LDFLAGS) $(LDLIBS) -o $@

install: tinywm
	mkdir -p "$(DESTDIR)$(PREFIX)/bin" "$(DESTDIR)$(PREFIX)/share/xsessions"
	install -m755 tinywm "$(DESTDIR)$(PREFIX)/bin/tinywm"
	install -m644 tinywm.desktop "$(DESTDIR)$(PREFIX)/share/xsessions/tinywm.desktop"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/tinywm"
	rm -f "$(DESTDIR)$(PREFIX)/share/xsessions/tinywm.desktop"

clean:
	rm -f tinywm

.PHONY: all install uninstall clean
