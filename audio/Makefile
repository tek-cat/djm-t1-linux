CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS   = -lusb-1.0 -lm

all: djmt1-capture djmt1-playback

djmt1-capture: djmt1-capture.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

djmt1-playback: djmt1-playback.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f djmt1-capture djmt1-playback

.PHONY: all clean
