CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
LDLIBS := $(shell pkg-config --libs libpulse 2>/dev/null)
CFLAGS += $(shell pkg-config --cflags libpulse 2>/dev/null)

.PHONY: all clean test

all: loudness-limiter loudness-limiter-ladspa.so test

loudness-limiter: src/loudness-limiter-native.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS) -lm -lpthread

loudness-limiter-ladspa.so: src/loudness-limiter-plugin.c
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $< -lm -lrt

test:
	$(CC) $(CFLAGS) -I. -o /tmp/loudness-limiter-test src/test-limiter.c -lm
	/tmp/loudness-limiter-test

clean:
	rm -f loudness-limiter loudness-limiter-ladspa.so
