CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS   = -lusb-1.0 -lasound
PREFIX  ?= /usr/local

all: djm_midi

djm_midi: djm_midi.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

install: djm_midi
	install -Dm755 djm_midi $(DESTDIR)$(PREFIX)/bin/djm_midi

# Per-user autostart service (NO sudo; goes in your home).
install-service:
	install -Dm644 systemd/djm-midi.service $(HOME)/.config/systemd/user/djm-midi.service
	@echo "enable with: systemctl --user daemon-reload && systemctl --user enable --now djm-midi"

clean:
	rm -f djm_midi

.PHONY: all install install-service clean
