CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
LDLIBS := $(shell pkg-config --libs libpulse 2>/dev/null)
CFLAGS += $(shell pkg-config --cflags libpulse 2>/dev/null)

.PHONY: all clean test install uninstall install-user uninstall-user

all: loudness-limiter test

loudness-limiter-ladspa.so: src/loudness-limiter-plugin.c
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $< -lm -lrt

loudness-limiter-plugin.o: loudness-limiter-ladspa.so
	ld -r -b binary -o $@ loudness-limiter-ladspa.so

loudness-limiter: src/loudness-limiter-native.c loudness-limiter-plugin.o
	$(CC) $(CFLAGS) -o $@ src/loudness-limiter-native.c loudness-limiter-plugin.o $(LDLIBS) -lm -lpthread
	rm -f loudness-limiter-ladspa.so loudness-limiter-plugin.o

test:
	$(CC) $(CFLAGS) -I. -o /tmp/loudness-limiter-test src/test-limiter.c -lm
	/tmp/loudness-limiter-test

clean:
	rm -f loudness-limiter loudness-limiter-ladspa.so loudness-limiter-plugin.o

install: loudness-limiter
	install -Dm755 loudness-limiter $(DESTDIR)/usr/local/bin/loudness-limiter
	install -Dm644 packaging/loudness-limiter.desktop $(DESTDIR)/usr/local/share/applications/loudness-limiter.desktop

install-user: loudness-limiter
	install -Dm755 loudness-limiter $(HOME)/.local/bin/loudness-limiter
	install -Dm644 packaging/loudness-limiter.desktop $(HOME)/.local/share/applications/loudness-limiter.desktop

uninstall-user:
	rm -f $(HOME)/.local/bin/loudness-limiter
	rm -f $(HOME)/.local/share/applications/loudness-limiter.desktop

uninstall:
	rm -f $(DESTDIR)/usr/local/bin/loudness-limiter
	rm -f $(DESTDIR)/usr/local/share/applications/loudness-limiter.desktop

.PHONY: autostart-enable autostart-disable autostart-status

autostart-enable: loudness-limiter
	./loudness-limiter --enable-autostart

autostart-disable: loudness-limiter
	./loudness-limiter --disable-autostart

autostart-status: loudness-limiter
	./loudness-limiter --autostart-status

.PHONY: default-limit default-limit-status default-limit-reset

default-limit: loudness-limiter
	@test -n "$(LIMIT)" || (echo '用法：make default-limit LIMIT=-24' >&2; exit 1)
	./loudness-limiter --set-default-limit $(LIMIT)

default-limit-status: loudness-limiter
	./loudness-limiter --get-default-limit

default-limit-reset: loudness-limiter
	./loudness-limiter --reset-default-limit
