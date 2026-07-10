CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS   = -lusb-1.0 -lasound
PREFIX  ?= /usr/local

PW_CFLAGS = $(shell pkg-config --cflags libpipewire-0.3 2>/dev/null)
PW_LIBS   = $(shell pkg-config --libs libpipewire-0.3 2>/dev/null)

all: djm_midi

djm_midi: djm_midi.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# Unified daemon: audio (PipeWire) + MIDI (ALSA seq) in one process.
# Needs libpipewire-0.3 dev; reuses the audio engine from ../audio/.
djm_full: djm_full.c ../audio/djmt1_iso.c ../audio/djmt1_iso.h
	$(CC) $(CFLAGS) $(PW_CFLAGS) -o $@ djm_full.c ../audio/djmt1_iso.c $(LDLIBS) -lpthread $(PW_LIBS)

install: djm_midi
	install -Dm755 djm_midi $(DESTDIR)$(PREFIX)/bin/djm_midi

# The unified daemon (audio + MIDI in one process); needs the PipeWire dev libs.
install-full: djm_full
	install -Dm755 djm_full $(DESTDIR)$(PREFIX)/bin/djm_full

# Per-user autostart service (NO sudo; goes in your home).
install-service:
	install -Dm644 systemd/djm-midi.service $(HOME)/.config/systemd/user/djm-midi.service
	@echo "enable with: systemctl --user daemon-reload && systemctl --user enable --now djm-midi"

# Per-user autostart for the unified daemon (replaces djmt1-audio + djm-midi).
install-full-service:
	install -Dm644 systemd/djm-full.service $(HOME)/.config/systemd/user/djm-full.service
	@echo "disable the split services first: systemctl --user disable --now djmt1-audio djm-midi"
	@echo "then enable: systemctl --user daemon-reload && systemctl --user enable --now djm-full"

clean:
	rm -f djm_midi djm_full

.PHONY: all install install-full install-service install-full-service clean
