# Next steps: extending the DJM-T1 support

Runbook for the open roadmap items. Items 1 to 3 are done at the mixer on Linux
(no VM needed) now that we can arm it; item 4 is a separate capture project.

**Prereqs:** build/install once (`make -C arm`, or `./install.sh`), plug the mixer
into a solid USB 2.0 port, and make sure it is armed (`sudo djm_arm`, or the udev
rule auto-arms it on plug-in). Confirm MIDI flows: `amidi -p hw:X,0,0 -d` then move
a fader.

## 1. LED / output feedback  (low effort)

Goal: light the mixer's LEDs (cue buttons, etc.) from Mixxx state.

```sh
sudo tools/probe-leds.sh          # sends each note to the mixer, one per ~1.5s
```
Watch the panel and note which note numbers light which LEDs (start by checking
whether the cue notes `0x2F`/`0x30` light the cue buttons). Report the mapping and
add matching `<output>` blocks to `mixxx/Pioneer-DJM-T1.midi.xml`.

## 2. Finish the MIDI map  (low effort)

Goal: map the FX section and any assignable buttons not yet covered.

```sh
sudo tools/capture-controls.sh 120     # arms + records for 2 min
#   ... move each unmapped control one at a time ...
tools/decode-midi.sh /tmp/djm-capture.txt
```
The decode table lists each distinct CC/Note. Add the new controls to the mapping.
Existing map: [midi-map.md](midi-map.md).

## 3. HID interface  (medium effort)

Goal: find out whether interface 3 (HID) carries anything MIDI doesn't.

```sh
sudo tools/probe-hid.sh 30
#   ... move controls / press buttons ...
```
If report bytes track a control that MIDI doesn't expose, that control lives on HID
and would need an HID mapping (Mixxx supports HID via a JS parser).

## 4. Built-in soundcard  (large effort, separate project)

Vendor-specific isochronous audio interface, no Linux driver. Full plan:
[audio-plan.md](audio-plan.md). Needs another VM + usbmon capture session, then a
driver. MIDI does not depend on this.

---

When you have results from 1 to 3, send them over and they get folded into the
mapping and docs.
