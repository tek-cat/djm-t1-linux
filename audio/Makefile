CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS   = -lusb-1.0 -lm
PTHREAD  = -lpthread

PW_CFLAGS = $(shell pkg-config --cflags libpipewire-0.3 2>/dev/null)
PW_LIBS   = $(shell pkg-config --libs libpipewire-0.3 2>/dev/null)

# CLI validation tools (no PipeWire needed)
all: djmt1-capture djmt1-playback djmt1-monitor

djmt1-capture: djmt1-capture.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

djmt1-playback: djmt1-playback.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

djmt1-monitor: djmt1-monitor.c djmt1_iso.c djmt1_iso.h
	$(CC) $(CFLAGS) -o $@ djmt1-monitor.c djmt1_iso.c $(LDLIBS) $(PTHREAD)

# The PipeWire audio device (needs libpipewire-0.3 dev)
djmt1-pipewire: djmt1-pipewire.c djmt1_iso.c djmt1_iso.h
	$(CC) $(CFLAGS) $(PW_CFLAGS) -o $@ djmt1-pipewire.c djmt1_iso.c $(LDLIBS) $(PTHREAD) $(PW_LIBS)

clean:
	rm -f djmt1-capture djmt1-playback djmt1-monitor djmt1-pipewire

.PHONY: all clean
