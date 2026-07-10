#!/usr/bin/env python3
# calibrate.py - a curses TUI to calibrate every Pioneer DJM-T1 control.
#
# Pick any control from the list, test it, and keep or redo the result. Each
# capture watches the live djm_midi bridge port and records what the control
# emits: message type, channel, CC/note number, value range, and the RESTING
# value (where you left it). For a detented control (crossfader, EQ, headphone
# mix), sweep the full range then settle at the center before stopping; the
# resting value is the true halfway point. That is the fix for "crossfader
# center sits slightly left in Mixxx": the physical detent does not land on
# MIDI 64, and the mapping can remap the measured center to 0.5.
#
# Results persist to tools/calibration.json after every capture, so you can quit
# and resume, and re-testing a control overwrites just that entry. A control you
# test that emits nothing is recorded as "silent" (distinct from "not tested"),
# which is useful for hardware-only controls.
#
# Design notes for reliability:
#   * ONE long-lived aseqdump for the whole session (no per-control re-subscribe,
#     which silently broke capture on the 2nd control in an earlier version).
#   * The reader auto-reconnects. The djm-midi service can restart and get a new
#     ALSA client number; the reader re-resolves the port and resubscribes, and
#     the header shows live connection status. Touches no service itself.
#
#   Usage:  python3 tools/calibrate.py            (auto-detect the bridge port)
#           python3 tools/calibrate.py --port 128:0
#
# SPDX-License-Identifier: MIT
import argparse
import curses
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(REPO, "tools", "calibration.json")
DETENT_HINT = "sweep full range, then settle at CENTER"

# ------------------------------------------------------------------ control list
# (id, section, label, hint, kind, detent)
CONTROLS = [
    ("utility",       "Global",  "UTILITY / WAKE UP button",           "press it",            "button", False),
    ("snap",          "Global",  "SNAP / QUANTIZE button",             "press it",            "button", False),
    ("shift",         "Global",  "SHIFT button",                       "press + release",     "button", False),

    ("ch1_source",    "CH1",     "CH1 source CD/PHONO/USB",            "slide through all 3", "switch", False),
    ("ch1_trim",      "CH1",     "CH1 TRIM",                           "full CCW to full CW", "knob",   False),
    ("ch1_hi",        "CH1",     "CH1 HI EQ",                          DETENT_HINT,           "knob",   True),
    ("ch1_mid",       "CH1",     "CH1 MID EQ",                         DETENT_HINT,           "knob",   True),
    ("ch1_low",       "CH1",     "CH1 LOW EQ",                         DETENT_HINT,           "knob",   True),
    ("ch1_cue",       "CH1",     "CH1 headphone CUE",                  "press it",            "button", False),
    ("ch1_fader",     "CH1",     "CH1 volume fader",                   "0 -> 10 -> 0",        "fader",  False),

    ("ch2_source",    "CH2",     "CH2 source CD/PHONO/USB",            "slide through all 3", "switch", False),
    ("ch2_trim",      "CH2",     "CH2 TRIM",                           "full CCW to full CW", "knob",   False),
    ("ch2_hi",        "CH2",     "CH2 HI EQ",                          DETENT_HINT,           "knob",   True),
    ("ch2_mid",       "CH2",     "CH2 MID EQ",                         DETENT_HINT,           "knob",   True),
    ("ch2_low",       "CH2",     "CH2 LOW EQ",                         DETENT_HINT,           "knob",   True),
    ("ch2_cue",       "CH2",     "CH2 headphone CUE",                  "press it",            "button", False),
    ("ch2_fader",     "CH2",     "CH2 volume fader",                   "0 -> 10 -> 0",        "fader",  False),

    ("crossfader",    "Master",  "Crossfader",                         DETENT_HINT,           "crossfader", True),
    ("xf_reverse",    "Master",  "CROSS F. REVERSE switch",            "toggle",              "switch", False),
    ("xf_feeling",    "Master",  "crossfader FEELING ADJ.",            "turn",                "knob",   False),
    ("phones_mix",    "Master",  "HEADPHONES MIXING",                  DETENT_HINT,           "knob",   True),
    ("phones_level",  "Master",  "HEADPHONES LEVEL",                   "full range",          "knob",   False),
    ("master_level",  "Master",  "MASTER LEVEL",                       "full range",          "knob",   False),
    ("booth_monitor", "Master",  "BOOTH MONITOR",                      "full range",          "knob",   False),

    ("browse_rotate", "Browse",  "BROWSE encoder rotate",              "CW then CCW",         "encoder", False),
    ("browse_push",   "Browse",  "BROWSE encoder PUSH",                "press in",            "button", False),

    ("load_a",        "Deck A",  "LOAD A",                             "press it",            "button", False),
    ("deckA_play",    "Deck A",  "DECK A PLAY/PAUSE",                  "press it",            "button", False),
    ("deckA_cue",     "Deck A",  "DECK A CUE",                         "press it",            "button", False),
    ("deckA_fx1",     "Deck A",  "DECK A FX1",                         "press it",            "button", False),
    ("deckA_fx2",     "Deck A",  "DECK A FX2",                         "press it",            "button", False),
    ("deckA_sync",    "Deck A",  "DECK A SYNC",                        "press it",            "button", False),

    ("load_b",        "Deck B",  "LOAD B",                             "press it",            "button", False),
    ("deckB_play",    "Deck B",  "DECK B PLAY/PAUSE",                  "press it",            "button", False),
    ("deckB_cue",     "Deck B",  "DECK B CUE",                         "press it",            "button", False),
    ("deckB_fx1",     "Deck B",  "DECK B FX1",                         "press it",            "button", False),
    ("deckB_fx2",     "Deck B",  "DECK B FX2",                         "press it",            "button", False),
    ("deckB_sync",    "Deck B",  "DECK B SYNC",                        "press it",            "button", False),

    ("fx1_on",        "FX1",     "FX1 ON (GROUP/SINGLE)",              "press it",            "button", False),
    ("fx1_drywet",    "FX1",     "FX1 DRY/WET",                        "full range",          "knob",   False),
    ("fx1_sel1",      "FX1",     "FX1 SELECT 1",                       "press it",            "button", False),
    ("fx1_sel2",      "FX1",     "FX1 SELECT 2",                       "press it",            "button", False),
    ("fx1_sel3",      "FX1",     "FX1 SELECT 3",                       "press it",            "button", False),
    ("fx1_p1",        "FX1",     "FX1 param 1",                        "full range",          "knob",   False),
    ("fx1_p2",        "FX1",     "FX1 param 2",                        "full range",          "knob",   False),
    ("fx1_p3",        "FX1",     "FX1 param 3",                        "full range",          "knob",   False),

    ("fx2_on",        "FX2",     "FX2 ON (GROUP/SINGLE)",              "press it",            "button", False),
    ("fx2_drywet",    "FX2",     "FX2 DRY/WET",                        "full range",          "knob",   False),
    ("fx2_sel1",      "FX2",     "FX2 SELECT 1",                       "press it",            "button", False),
    ("fx2_sel2",      "FX2",     "FX2 SELECT 2",                       "press it",            "button", False),
    ("fx2_sel3",      "FX2",     "FX2 SELECT 3",                       "press it",            "button", False),
    ("fx2_p1",        "FX2",     "FX2 param 1",                        "full range",          "knob",   False),
    ("fx2_p2",        "FX2",     "FX2 param 2",                        "full range",          "knob",   False),
    ("fx2_p3",        "FX2",     "FX2 param 3",                        "full range",          "knob",   False),

    ("ac_loop_rot",   "Deck A/C","A/C AUTO LOOP rotate",               "CW then CCW",         "encoder", False),
    ("ac_loop_push",  "Deck A/C","A/C AUTO LOOP push",                 "press in",            "button", False),
    ("ac_select",     "Deck A/C","A/C SELECT (CROSS.F.CONTROL)",       "press it",            "button", False),
    ("ac_hotcue1",    "Deck A/C","A/C HOT CUE 1",                      "press it",            "button", False),
    ("ac_hotcue2",    "Deck A/C","A/C HOT CUE 2",                      "press it",            "button", False),
    ("ac_hotcue3",    "Deck A/C","A/C HOT CUE 3",                      "press it",            "button", False),
    ("ac_hotcue4",    "Deck A/C","A/C HOT CUE 4",                      "press it",            "button", False),
    ("ac_samp1",      "Deck A/C","A/C SAMPLER 1",                      "press it",            "button", False),
    ("ac_samp2",      "Deck A/C","A/C SAMPLER 2",                      "press it",            "button", False),
    ("ac_samp3",      "Deck A/C","A/C SAMPLER 3",                      "press it",            "button", False),
    ("ac_samp4",      "Deck A/C","A/C SAMPLER 4",                      "press it",            "button", False),
    ("ac_active",     "Deck A/C","A/C ACTIVE",                         "press it",            "button", False),
    ("ac_playmode",   "Deck A/C","A/C PLAY MODE switch",               "toggle",              "switch", False),

    ("cd_loop_rot",   "Deck C/D","C/D AUTO LOOP rotate",               "CW then CCW",         "encoder", False),
    ("cd_loop_push",  "Deck C/D","C/D AUTO LOOP push",                 "press in",            "button", False),
    ("cd_select",     "Deck C/D","C/D SELECT (CROSS.F.CONTROL)",       "press it",            "button", False),
    ("cd_hotcue1",    "Deck C/D","C/D HOT CUE 1",                      "press it",            "button", False),
    ("cd_hotcue2",    "Deck C/D","C/D HOT CUE 2",                      "press it",            "button", False),
    ("cd_hotcue3",    "Deck C/D","C/D HOT CUE 3",                      "press it",            "button", False),
    ("cd_hotcue4",    "Deck C/D","C/D HOT CUE 4",                      "press it",            "button", False),
    ("cd_samp1",      "Deck C/D","C/D SAMPLER 1",                      "press it",            "button", False),
    ("cd_samp2",      "Deck C/D","C/D SAMPLER 2",                      "press it",            "button", False),
    ("cd_samp3",      "Deck C/D","C/D SAMPLER 3",                      "press it",            "button", False),
    ("cd_samp4",      "Deck C/D","C/D SAMPLER 4",                      "press it",            "button", False),
    ("cd_active",     "Deck C/D","C/D ACTIVE",                         "press it",            "button", False),
    ("cd_playmode",   "Deck C/D","C/D PLAY MODE switch",               "toggle",              "switch", False),
]

# ------------------------------------------------------------------ port + parse
def find_bridge_port():
    """(port, name) for the live djm_midi bridge, preferring the user-type client
    over the (silent) kernel port. (None, reason) if not present."""
    try:
        out = subprocess.check_output(["aconnect", "-i"], text=True, stderr=subprocess.DEVNULL)
    except Exception:
        return None, "aconnect unavailable"
    user_hit = any_hit = None
    for line in out.splitlines():
        m = re.match(r"client (\d+): '([^']*)' \[type=(\w+)", line)
        if m and "DJM-T1" in m.group(2).upper():
            hit = (m.group(1), m.group(2))
            any_hit = any_hit or hit
            if m.group(3) == "user":
                user_hit = user_hit or hit
    hit = user_hit or any_hit
    if not hit:
        return None, "bridge not found"
    return f"{hit[0]}:0", f"{hit[1]} ({hit[0]}:0)"

RE = {
    "cc":  re.compile(r"Control change\s+(\d+), controller (\d+), value (\d+)"),
    "on":  re.compile(r"Note on\s+(\d+), note (\d+), velocity (\d+)"),
    "off": re.compile(r"Note off\s+(\d+), note (\d+), velocity (\d+)"),
    "pb":  re.compile(r"Pitchbend\s+(\d+), value (-?\d+)"),
    "pgm": re.compile(r"Program change\s+(\d+), program (\d+)"),
}

def parse(line):
    m = RE["cc"].search(line)
    if m:  return ("cc", int(m.group(1)), int(m.group(2)), int(m.group(3)))
    m = RE["on"].search(line)
    if m:  return ("note_on", int(m.group(1)), int(m.group(2)), int(m.group(3)))
    m = RE["off"].search(line)
    if m:  return ("note_off", int(m.group(1)), int(m.group(2)), int(m.group(3)))
    m = RE["pb"].search(line)
    if m:  return ("pitchbend", int(m.group(1)), None, int(m.group(2)))
    m = RE["pgm"].search(line)
    if m:  return ("program", int(m.group(1)), int(m.group(2)), None)
    return None

def aggregate(events):
    agg, order = {}, []
    for etype, ch, num, val in events:
        key = (etype, ch, num)
        if key not in agg:
            agg[key] = {"type": etype, "channel": ch, "number": num,
                        "count": 0, "vmin": None, "vmax": None, "last": None}
            order.append(key)
        a = agg[key]
        a["count"] += 1
        if val is not None:
            a["vmin"] = val if a["vmin"] is None else min(a["vmin"], val)
            a["vmax"] = val if a["vmax"] is None else max(a["vmax"], val)
            a["last"] = val
    return [agg[k] for k in order]

# ------------------------------------------------------------------ reader thread
class Reader(threading.Thread):
    """One aseqdump, kept alive. Re-resolves and reconnects if the bridge port
    goes away (service restart). Thread-safe event buffer."""
    def __init__(self, forced_port=None):
        super().__init__(daemon=True)
        self.forced = forced_port
        self.lock = threading.Lock()
        self.events = []
        self.proc = None
        self.alive = True
        self.connected = False
        self.port = forced_port
        self.status = "starting"

    def _sb_cmd(self, port):
        cmd = ["aseqdump", "-p", port]
        if shutil.which("stdbuf"):
            cmd = ["stdbuf", "-oL", "-eL"] + cmd
        return cmd

    def run(self):
        while self.alive:
            if self.forced:
                port, name = self.forced, self.forced
            else:
                port, name = find_bridge_port()
            if not port:
                self.connected = False
                self.port = None
                self.status = name  # reason
                time.sleep(0.6)
                continue
            self.port, self.status = name, "connected"
            try:
                self.proc = subprocess.Popen(self._sb_cmd(port), stdout=subprocess.PIPE,
                                             stderr=subprocess.STDOUT, text=True, bufsize=1)
            except FileNotFoundError:
                self.status = "aseqdump missing"
                self.connected = False
                time.sleep(1.0)
                continue
            self.connected = True
            try:
                for line in self.proc.stdout:
                    if not self.alive:
                        break
                    ev = parse(line)
                    if ev:
                        with self.lock:
                            self.events.append(ev)
            except Exception:
                pass
            self.connected = False
            if not self.alive:
                break
            self.status = "reconnecting"
            time.sleep(0.4)

    def clear(self):
        with self.lock:
            self.events.clear()

    def snapshot(self):
        with self.lock:
            return list(self.events)

    def stop(self):
        self.alive = False
        if self.proc:
            try:
                self.proc.terminate()
            except Exception:
                pass

# ------------------------------------------------------------------ persistence
def load_results():
    try:
        with open(OUT) as f:
            return json.load(f).get("controls", {})
    except Exception:
        return {}

def save_results(results, port_desc):
    data = {"port": port_desc,
            "captured_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "controls": results}
    tmp = OUT + ".tmp"
    try:
        with open(tmp, "w") as f:
            json.dump(data, f, indent=2)
        os.replace(tmp, OUT)
        return True
    except Exception:
        return False

def build_record(control, agg):
    cid, section, label, hint, kind, detent = control
    rec = {"label": label, "section": section, "kind": kind, "detent": detent,
           "tested": True, "captured": bool(agg), "events": agg,
           "captured_at": datetime.now(timezone.utc).isoformat(timespec="seconds")}
    ccs = [a for a in agg if a["type"] == "cc"]
    if ccs:
        prim = max(ccs, key=lambda a: a["count"])
        rec["primary_cc"] = prim["number"]
        rec["resting"] = prim["last"]
        if detent and prim["last"] is not None:
            rec["center_offset"] = prim["last"] - 64
    notes = [a["number"] for a in agg if a["type"] in ("note_on", "note_off")]
    if notes:
        rec["note_numbers"] = sorted(set(notes))
    return rec

def summary_str(rec):
    if not rec or not rec.get("tested"):
        return "· not tested"
    if not rec.get("captured"):
        return "— silent (no MIDI)"
    agg = rec.get("events", [])
    ccs = [a for a in agg if a["type"] == "cc"]
    if ccs:
        prim = max(ccs, key=lambda a: a["count"])
        s = f"CC{prim['number']} {prim['vmin']}..{prim['vmax']}"
        if rec.get("resting") is not None:
            s += f" @{rec['resting']}"
            if rec.get("center_offset") is not None:
                s += f" (off {rec['center_offset']:+d})"
        others = sorted({a["number"] for a in ccs if a["number"] != prim["number"]})
        if others:
            s += " +CC" + ",".join(str(o) for o in others)
        return s
    notes = sorted({a["number"] for a in agg if a["type"] in ("note_on", "note_off")})
    if notes:
        return "Note " + ",".join(f"{n}(0x{n:02X})" for n in notes)
    return agg[0]["type"] if agg else "?"

# ------------------------------------------------------------------ curses helpers
_have_color = False
def color(n):
    return curses.color_pair(n) if _have_color else 0

def safe(win, y, x, text, attr=0):
    h, w = win.getmaxyx()
    if 0 <= y < h and 0 <= x < w - 1:
        try:
            win.addnstr(y, x, text, w - x - 1, attr)
        except curses.error:
            pass

def too_small(win, minh=8, minw=44):
    h, w = win.getmaxyx()
    if h < minh or w < minw:
        win.erase()
        safe(win, 0, 0, "Terminal too small.")
        safe(win, 1, 0, f"Need >= {minw}x{minh}, have {w}x{h}.")
        win.refresh()
        return True
    return False

def build_rows():
    rows, last = [], None
    for c in CONTROLS:
        if c[1] != last:
            rows.append(("header", c[1]))
            last = c[1]
        rows.append(("control", c))
    return rows

# ------------------------------------------------------------------ help overlay
HELP_LINES = [
    "DJM-T1 calibration  -  keys",
    "",
    "  up / down / j / k    move selection",
    "  PgUp / PgDn          jump a page",
    "  Home / End           first / last",
    "  ENTER or SPACE       test the selected control",
    "  n                    jump to next untested control",
    "  d                    clear the selected control's result",
    "  ?                    this help",
    "  q                    quit (results are already saved)",
    "",
    "During a test:",
    "  move / press the control; events stream live",
    "  for a detented control, settle at center before stopping",
    "  SPACE  stop and review     ESC  cancel",
    "Review:",
    "  s  save (overwrite)   r  redo   ESC  cancel",
    "",
    "press any key to close",
]

def show_help(stdscr):
    stdscr.timeout(-1)
    while True:
        stdscr.erase()
        if not too_small(stdscr, minh=len(HELP_LINES) + 2, minw=46):
            for i, ln in enumerate(HELP_LINES):
                attr = curses.A_BOLD | color(3) if i == 0 else 0
                safe(stdscr, 1 + i, 2, ln, attr)
            stdscr.refresh()
        ch = stdscr.getch()
        if ch != curses.KEY_RESIZE:
            return

# ------------------------------------------------------------------ capture flow
def draw_capture(stdscr, control, agg, phase, reader, msg=""):
    cid, section, label, hint, kind, detent = control
    stdscr.erase()
    if too_small(stdscr):
        return
    h, w = stdscr.getmaxyx()
    safe(stdscr, 0, 2, f"TEST  {section} :: {label}", curses.A_BOLD | color(3))
    safe(stdscr, 1, 2, f"do: {hint}", curses.A_DIM)
    if not reader.connected:
        safe(stdscr, 1, w - 24, "[reader offline]", curses.A_BOLD | color(4))
    y = 3
    if not agg:
        safe(stdscr, y, 4, "... move the control now; events appear here ...", curses.A_DIM)
        y += 1
    for a in agg:
        if y >= h - 2:
            break
        if a["type"] == "cc":
            line = (f"CC {a['number']:>3} (0x{a['number']:02X})   {a['count']:>4} msgs   "
                    f"range {a['vmin']}..{a['vmax']}   now {a['last']}")
            if detent and a["last"] is not None:
                line += f"   center {a['last']-64:+d} vs 64"
            attr = color(2)
        elif a["type"] in ("note_on", "note_off"):
            t = "Note-on " if a["type"] == "note_on" else "Note-off"
            line = f"{t} {a['number']:>3} (0x{a['number']:02X})   {a['count']:>3}x   vel {a['vmin']}..{a['vmax']}"
            attr = color(4)
        elif a["type"] == "pitchbend":
            line = f"Pitchbend   {a['count']:>3}x   {a['vmin']}..{a['vmax']}"
            attr = color(4)
        else:
            line = f"{a['type']} {a['number']}"
            attr = 0
        safe(stdscr, y, 4, line, attr)
        y += 1
    if msg:
        safe(stdscr, h - 2, 2, msg, curses.A_BOLD | color(4))
    if phase == "capture":
        foot = " capturing:  move/press the control    SPACE = stop & review    ESC = cancel "
    elif phase == "empty":
        foot = " no MIDI seen:  s = save as SILENT    r = keep trying    ESC = cancel "
    else:
        foot = " review:  s = SAVE (overwrite)    r = redo    ESC = cancel "
    safe(stdscr, h - 1, 0, foot.ljust(w - 1), curses.A_REVERSE)
    stdscr.refresh()

def do_capture(stdscr, reader, control):
    """Live capture -> review. Returns a record to save, or None if cancelled."""
    while True:
        reader.clear()
        stdscr.timeout(120)
        agg = []
        stopped = False
        while not stopped:
            agg = aggregate(reader.snapshot())
            draw_capture(stdscr, control, agg, "capture", reader)
            ch = stdscr.getch()
            if ch in (ord(" "), ord("\n"), curses.KEY_ENTER, 10, 13):
                stopped = True
            elif ch == 27:
                return None
            # KEY_RESIZE and -1 (timeout) just fall through and redraw
        phase = "review" if agg else "empty"
        while True:
            draw_capture(stdscr, control, agg, phase, reader)
            ch = stdscr.getch()
            if ch in (ord("s"), ord("S")) or (agg and ch in (ord("\n"), curses.KEY_ENTER, 10, 13)):
                return build_record(control, agg)
            if ch in (ord("r"), ord("R")):
                break
            if ch in (27, ord("c"), ord("C"), ord("q")):
                return None

# ------------------------------------------------------------------ list screen
def counts(results):
    cap = sil = 0
    for c in CONTROLS:
        r = results.get(c[0])
        if r and r.get("tested"):
            if r.get("captured"):
                cap += 1
            else:
                sil += 1
    return cap, sil, len(CONTROLS) - cap - sil

def draw_list(stdscr, rows, sel_row, results, top, reader, flash=""):
    stdscr.erase()
    if too_small(stdscr):
        return
    h, w = stdscr.getmaxyx()
    cap, sil, left = counts(results)
    head = f"DJM-T1 calibration   {cap} captured  {sil} silent  {left} left"
    safe(stdscr, 0, 2, head, curses.A_BOLD | color(3))
    stat = "connected" if reader.connected else reader.status
    sattr = color(2) if reader.connected else color(4)
    port = reader.port or "-"
    safe(stdscr, 0, max(2, w - len(f"{stat}  {port}") - 3), f"{stat}  {port}", sattr)
    view_h = h - 2
    for i in range(view_h):
        ridx = top + i
        if ridx >= len(rows):
            break
        y = 1 + i
        kind, payload = rows[ridx]
        if kind == "header":
            safe(stdscr, y, 1, f"-- {payload} --", curses.A_BOLD | color(5))
            continue
        cid, _, label = payload[0], payload[1], payload[2]
        rec = results.get(cid)
        sel = ridx == sel_row
        row = f" {label:<30} {summary_str(rec)}"
        if sel:
            safe(stdscr, y, 2, row.ljust(w - 4), curses.A_REVERSE)
        else:
            attr = color(2) if (rec and rec.get("captured")) else (curses.A_DIM if (rec and rec.get("tested")) else 0)
            safe(stdscr, y, 2, row, attr)
    if top > 0:
        safe(stdscr, 1, w - 3, "^", curses.A_BOLD)
    if top + view_h < len(rows):
        safe(stdscr, h - 2, w - 3, "v", curses.A_BOLD)
    foot = flash or " up/down move   ENTER test   n next-untested   d clear   ? help   q quit "
    safe(stdscr, h - 1, 0, foot.ljust(w - 1), curses.A_REVERSE)
    stdscr.refresh()

def run_tui(stdscr, reader, results, save_desc):
    global _have_color
    try:
        curses.curs_set(0)
    except curses.error:
        pass
    try:
        curses.start_color()
        curses.use_default_colors()
        curses.init_pair(2, curses.COLOR_GREEN, -1)
        curses.init_pair(3, curses.COLOR_CYAN, -1)
        curses.init_pair(4, curses.COLOR_YELLOW, -1)
        curses.init_pair(5, curses.COLOR_MAGENTA, -1)
        _have_color = True
    except curses.error:
        _have_color = False

    rows = build_rows()
    ctl_rows = [i for i, r in enumerate(rows) if r[0] == "control"]
    sel = 0
    top = 0
    flash = ""
    while True:
        h, _ = stdscr.getmaxyx()
        sel_row = ctl_rows[sel]
        view_h = max(1, h - 2)
        if sel_row < top + 1:
            top = max(0, sel_row - 1)
        if sel_row > top + view_h - 1:
            top = sel_row - view_h + 1
        stdscr.timeout(-1)
        draw_list(stdscr, rows, sel_row, results, top, reader, flash)
        flash = ""
        ch = stdscr.getch()
        if ch in (ord("q"), ord("Q")):
            break
        elif ch in (curses.KEY_DOWN, ord("j")):
            sel = min(len(ctl_rows) - 1, sel + 1)
        elif ch in (curses.KEY_UP, ord("k")):
            sel = max(0, sel - 1)
        elif ch == curses.KEY_NPAGE:
            sel = min(len(ctl_rows) - 1, sel + max(1, view_h // 2))
        elif ch == curses.KEY_PPAGE:
            sel = max(0, sel - max(1, view_h // 2))
        elif ch in (curses.KEY_HOME, ord("g")):
            sel = 0
        elif ch in (curses.KEY_END, ord("G")):
            sel = len(ctl_rows) - 1
        elif ch == ord("?"):
            show_help(stdscr)
        elif ch == ord("n"):
            nxt = next((k for k in range(sel + 1, len(ctl_rows)) if not results.get(CONTROLS[k][0], {}).get("tested")), None)
            if nxt is None:
                nxt = next((k for k in range(0, len(ctl_rows)) if not results.get(CONTROLS[k][0], {}).get("tested")), None)
            if nxt is None:
                flash = " all controls tested "
            else:
                sel = nxt
        elif ch in (ord("d"), curses.KEY_DC):
            cid = CONTROLS[sel][0]
            if results.pop(cid, None) is not None:
                save_results(results, save_desc)
                flash = f" cleared {CONTROLS[sel][2]} "
        elif ch in (ord("\n"), curses.KEY_ENTER, 10, 13, ord(" ")):
            control = rows[sel_row][1]
            rec = do_capture(stdscr, reader, control)
            if rec is not None:
                results[control[0]] = rec
                ok = save_results(results, save_desc)
                flash = (" saved " if ok else " SAVE FAILED ") + f"{control[2]}: {summary_str(rec)} "
        # KEY_RESIZE and any other key: loop and redraw

def main():
    ap = argparse.ArgumentParser(
        description="Curses TUI to calibrate the Pioneer DJM-T1 controls into tools/calibration.json.",
        epilog="Requires the djm_midi bridge running (djm-midi.service) and ALSA's aseqdump.")
    ap.add_argument("--port", help="force an ALSA seq port, e.g. 128:0 (default: auto-detect)")
    args = ap.parse_args()

    if not shutil.which("aseqdump"):
        print("ERROR: aseqdump not found (install alsa-utils).", file=sys.stderr)
        sys.exit(1)

    save_desc = args.port or (find_bridge_port()[1])
    reader = Reader(forced_port=args.port)
    reader.start()
    time.sleep(0.4)

    results = load_results()
    try:
        curses.wrapper(run_tui, reader, results, save_desc)
    except KeyboardInterrupt:
        pass
    finally:
        reader.stop()
        save_results(results, save_desc)
    cap, sil, left = counts(results)
    print(f"Saved to {OUT}:  {cap} captured, {sil} silent, {left} untested.")

if __name__ == "__main__":
    main()
