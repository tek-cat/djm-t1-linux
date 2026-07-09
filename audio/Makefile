CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS   = -lusb-1.0 -lm
PTHREAD  = -lpthread

all: djmt1-capture djmt1-playback djmt1-monitor

djmt1-capture: djmt1-capture.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

djmt1-playback: djmt1-playback.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

djmt1-monitor: djmt1-monitor.c djmt1_iso.c djmt1_iso.h
	$(CC) $(CFLAGS) -o $@ djmt1-monitor.c djmt1_iso.c $(LDLIBS) $(PTHREAD)

clean:
	rm -f djmt1-capture djmt1-playback djmt1-monitor

.PHONY: all clean
