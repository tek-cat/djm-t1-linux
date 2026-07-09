CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS   = -lusb-1.0 -lm

djmt1-capture: djmt1-capture.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f djmt1-capture

.PHONY: clean
