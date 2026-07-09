CC      ?= cc
CFLAGS  ?= -O2 -Wall
PREFIX  ?= /usr/local

djm_arm: djm_arm.c
	$(CC) $(CFLAGS) -o $@ $< -lusb-1.0

install: djm_arm
	install -Dm755 djm_arm $(DESTDIR)$(PREFIX)/bin/djm_arm

clean:
	rm -f djm_arm

.PHONY: install clean
