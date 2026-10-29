"""Runs one YAML test live and writes runs/<date>-<test>/result.json.

Steps (one key per list item):
  wait_s: 2.5                       sleep (frame-accurate waits arrive with item 3)
  key: esc | {key: w, hold: 1.5}    tap / hold a key (scan code)
  type: "fd_fov 90"                 type text as scan codes
  click: [x, y]                     left click at client coords
  focus: true                       bring the game window to the foreground
  screenshot: name                  checkpoint image (+ non-black luma recorded)
  assert_mem: {name, equals|not_equals|min|max, tolerance}
  sample_mem: {name, as: label}     record a value without asserting (for comparisons)
  assert_log: {contains: "text"}    FDNYCore.log must contain text
  console: "fd_fov 90"              open the game console, type, Enter, close (item 4+)
"""
import ctypes
import ctypes.wintypes as wt
import datetime
import json
import math
import os
import re
import shutil
import struct
import sys
import time

import yaml

from . import addrtable, capture, game, inputs, pipe, session

REPO_DIR = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
RUNS_DIR = os.path.join(REPO_DIR, "build", "runs")
SETUPS_DIR = os.path.join(REPO_DIR, "tools", "harness", "setups")
CORE_ASI = "FDNYCore.asi"


def core_version():
    """VERSION: the single source build.ps1 compiles into the core."""
    with open(os.path.join(REPO_DIR, "VERSION"), encoding="ascii") as f:
        return f.read().strip()


def deploy_core(game_dir, config):
    """config: 'Debug' | 'Release' | None (core removed for baseline runs)."""
    dst = os.path.join(game_dir, CORE_ASI)
    if config is None:
        if os.path.exists(dst):
            os.replace(dst, dst + ".off")  # ASI loader only loads *.asi
        return
    shutil.copy2(os.path.join(REPO_DIR, "build", config, CORE_ASI), dst)


class Run:
    def __init__(self, test_path, game_dir, core_config, tag="", session=None):
        with open(test_path, encoding="utf-8") as f:
            self.test = yaml.safe_load(f)
        self.session = session  # fdh.session.Session: a running game shared with other tests
        self.log_from = 0       # in a session: where this test's part of FDNYCore.log starts
        self.test_path, self.tag = test_path, tag
        self.game_dir = game_dir
        # A test may name the build it needs (`core: Release`: what players get); `--core none`
        # (a baseline run) still removes the core.
        self.core_config = self.test.get("core", core_config) if core_config is not None else None
        stamp = datetime.datetime.now().strftime("%Y-%m-%d-%H%M%S")
        suffix = f"-{tag}" if tag else ""
        self.out = os.path.join(RUNS_DIR, f"{stamp}-{self.test['name']}{suffix}")
        os.makedirs(self.out, exist_ok=True)
        self.results, self.samples = [], {}
        self.g = game.Game(game_dir)
        self.resolver = None

    def record(self, kind, ok, detail):
        self.results.append({"step": kind, "pass": bool(ok), "detail": detail})
        print(f"  [{'PASS' if ok else 'FAIL'}] {kind}: {detail}")
        return ok

    # --- steps -------------------------------------------------------------
    def client_point(self, x, y):
        cx, cy, _, _ = self.g.client_rect()
        return cx + x, cy + y

    def step(self, s):
        (kind, arg), = s.items()
        if kind == "wait_s":
            time.sleep(float(arg))
        elif kind == "focus":
            self.record("focus", inputs.focus(self.g.hwnd), "game window foreground")
        elif kind == "key":
            if isinstance(arg, dict):
                inputs.key_down(arg["key"])
                time.sleep(float(arg.get("hold", 0.05)))
                inputs.key_up(arg["key"])
            else:
                inputs.tap(arg)
        elif kind == "key_down":
            inputs.key_down(arg)
        elif kind == "key_up":
            inputs.key_up(arg)
        elif kind == "type":
            inputs.type_text(str(arg))
        elif kind == "click":
            inputs.click(*self.client_point(*arg))
        elif kind == "mouse_rel":
            inputs.mouse_move_rel(*arg)
        elif kind == "rclick":
            inputs.mouse_click_here(right=True)
        elif kind == "gclick":
            # Game-cursor click: the menus track their own cursor from DirectInput
            # deltas (1:1, clamped to the screen), so slam to the top-left corner,
            # then move by the target coords.
            inputs.mouse_move_rel(-3000, -3000)
            time.sleep(0.15)
            inputs.mouse_move_rel(*arg)
            time.sleep(0.25)
            # The click lands wherever the OS cursor is. Relative moves can leave
            # it outside the game window (e.g. if it was warped mid-move), and a
            # stray click on the taskbar opened the Start menu (2026-10-21). An
            # absolute SetCursorPos yields no DirectInput delta, so park the OS
            # cursor inside the window: the game's own menu cursor stays put.
            cx, cy, cw, ch = self.g.client_rect()
            inputs.set_cursor(cx + cw // 2, cy + ch // 2)
            time.sleep(0.1)
            inputs.mouse_click_here()
        elif kind == "screenshot":
            path, luma = capture.screenshot(self.g.client_rect(), os.path.join(self.out, f"{arg}.png"))
            self.samples[f"screenshot.{arg}.luma"] = round(luma, 1)
        elif kind == "assert_screen_diff":
            # {a, b, box: [x0, y0, x1, y1] (fractions of the shot, default all), min|max}: mean
            # absolute RGB difference (0-255) between two screenshots of this run over the box
            from PIL import Image, ImageChops, ImageStat
            ia = Image.open(os.path.join(self.out, f"{arg['a']}.png")).convert("RGB")
            ib = Image.open(os.path.join(self.out, f"{arg['b']}.png")).convert("RGB").resize(ia.size)
            fx0, fy0, fx1, fy1 = arg.get("box", [0, 0, 1, 1])
            w, h = ia.size
            crop = (int(fx0 * w), int(fy0 * h), int(fx1 * w), int(fy1 * h))
            d = sum(ImageStat.Stat(ImageChops.difference(ia.crop(crop), ib.crop(crop))).mean) / 3
            ok = ("min" not in arg or d >= float(arg["min"])) and ("max" not in arg or d <= float(arg["max"]))
            want = " and ".join(([f">= {arg['min']}"] if "min" in arg else []) + ([f"<= {arg['max']}"] if "max" in arg else []))
            self.record(kind, ok, f"{arg['a']} vs {arg['b']} over {arg.get('box', 'all')}: mean diff {d:.2f}, want {want}")
        elif kind == "assert_screen_nonblack":
            path, luma = capture.screenshot(self.g.client_rect(), os.path.join(self.out, f"{arg}.png"))
            self.record("assert_screen_nonblack", luma > 8, f"{arg} mean luma {luma:.1f}")
        elif kind == "repeat":
            for _ in range(int(arg["times"])):
                for sub in arg["steps"]:
                    if self.g.check_crash():
                        return
                    self.step(sub)
        elif kind == "pipe":
            self.samples.setdefault("pipe", []).append([arg, pipe.call(arg)])
        elif kind == "assert_frame_hooks":
            self.assert_frame_hooks(float(arg.get("seconds", 5)), int(arg.get("tolerance", 2)))
        elif kind in ("assert_same_pos", "assert_moved"):
            x, z = self.resolver.read("player.pos_x"), self.resolver.read("player.pos_z")
            x0, z0 = self.samples[arg["x"]], self.samples[arg["z"]]
            d = ((x - x0) ** 2 + (z - z0) ** 2) ** 0.5
            if kind == "assert_same_pos":
                self.record(kind, d <= float(arg["tolerance"]),
                            f"player moved {d:.1f} u (must be <= {arg['tolerance']})")
            else:
                self.record(kind, d >= float(arg["min"]), f"player moved {d:.1f} u (must be >= {arg['min']})")
        elif kind == "wait_log":
            # {contains, timeout}: wait for one more occurrence of the text in FDNYCore.log
            # than there was when the step started (for events with no fixed timing).
            # since_console: true counts from the start of the last `console` step instead,
            # for a line the command itself may already have produced.
            def count():
                return self.read_core_log().count(arg["contains"])
            n0, t0, timeout = count(), time.time(), float(arg.get("timeout", 30))
            if arg.get("since_console"):
                n0 = getattr(self, "console_log_text", "").count(arg["contains"])
            while count() <= n0 and time.time() - t0 < timeout:
                time.sleep(0.1)
            seen = count() > n0
            self.record("wait_log", seen, f"{arg['contains']!r} {'after' if seen else 'not within'} "
                                          f"{time.time() - t0:.1f} s")
        elif kind == "wait_pipe":
            # {cmd, key, equals, timeout}: poll a numeric pipe field until it equals the value
            # (for state that settles a frame or two after a hook, on a VM that can hitch)
            t0, timeout, val = time.time(), float(arg.get("timeout", 3)), None
            while True:
                reply = dict(kv.split("=", 1) for kv in pipe.call(arg["cmd"]).split() if "=" in kv)
                val = int(reply.get(arg["key"], "-1"))  # missing: -1, never a wanted value
                if val == int(arg["equals"]) or time.time() - t0 >= timeout:
                    break
                time.sleep(0.05)
            self.record(kind, val == int(arg["equals"]),
                        f"{arg['cmd']}.{arg['key']} = {val} after {time.time() - t0:.2f} s, want {arg['equals']}")
        elif kind == "assert_juggle":
            self.assert_juggle(arg if isinstance(arg, dict) else {})
        elif kind == "assert_hooks_idle":
            # Hook group switched off: its counters must not move while the game renders.
            group = arg.get("group", "frame")
            self.record("hook_off", pipe.call(f"hook {group} off") == "ok", f"{group} disabled")
            s0 = pipe.stats()
            time.sleep(float(arg.get("seconds", 2)))
            s1 = pipe.stats()
            d = {k: s1[k] - s0[k] for k in ("begin", "end", "events")}
            self.record("hooks_idle_while_off", all(v == 0 for v in d.values()),
                        f"{group} off for {arg.get('seconds', 2)}s: deltas {d}")
            self.record("hook_on", pipe.call(f"hook {group} on") == "ok", f"{group} re-enabled")
        elif kind == "toggle_hooks":
            fails = 0
            for _ in range(int(arg.get("times", 50))):
                fails += pipe.call(f"hook {arg['group']} off") != "ok"
                time.sleep(float(arg.get("gap_s", 0.05)))
                fails += pipe.call(f"hook {arg['group']} on") != "ok"
                time.sleep(float(arg.get("gap_s", 0.05)))
            self.record("toggle_hooks", fails == 0,
                        f"{arg.get('times', 50)}x off/on '{arg['group']}', {fails} failed command(s)")
        elif kind == "sample_addr":
            # Resolved address of each named entry (autosplitter-safety check).
            for name in arg:
                a = self.resolver.address(name)
                self.samples[f"addr.{name}"] = hex(a) if a is not None else None
        elif kind == "sample_pos":
            self.samples[f"pos.{arg}"] = [round(self.resolver.read(f"player.pos_{c}"), 2) for c in "xyz"]
            self.samples[f"t.{arg}"] = round(time.time() - self.g.started, 2)
        elif kind in ("assert_mem", "sample_mem"):
            val = self.resolver.read(arg["name"])
            if kind == "sample_mem":
                key = arg.get("as", arg["name"])
                if arg.get("append"):
                    self.samples.setdefault(key, []).append(val)
                else:
                    self.samples[key] = val
                return
            self.assert_value(arg, val)
        elif kind == "restart":
            # Kill and relaunch inside one test (persistence checks); setup is not re-run.
            self.g.kill()
            time.sleep(1.0)
            self.g.launch(self.test.get("env"))
            crash = self.g.wait_window()
            if not crash:
                time.sleep(1.0)
                inputs.focus(self.g.hwnd)
                crash = self.g.wait_ready() if self.core_config else None
            self.record("restart", crash is None, crash or "relaunched; core ready")
            if not crash:
                self.resolver.pm = None
                bases = self.g.attach()
                self.resolver = addrtable.Resolver(self.g.pm, self.resolver_version, bases)
                if arg.get("wait_s"):
                    time.sleep(float(arg["wait_s"]))
        elif kind == "alt_tab":
            inputs.alt_tab()
            time.sleep(float(arg.get("wait_s", 1.5)) if isinstance(arg, dict) else 1.5)
        elif kind == "assert_foreground":
            is_game = inputs.foreground() == self.g.hwnd
            self.record("assert_foreground", is_game == bool(arg),
                        f"game {'is' if is_game else 'is not'} the foreground window (expected {'game' if arg else 'other'})")
        elif kind == "set_cursor":
            inputs.set_cursor(*arg)
        elif kind == "assert_cursor":
            x, y = inputs.cursor_pos()
            if arg.get("window_centre"):
                r = wt.RECT()
                ctypes.windll.user32.GetWindowRect(self.g.hwnd, ctypes.byref(r))
                ex, ey = (r.left + r.right) >> 1, (r.top + r.bottom) >> 1
            else:
                ex, ey = arg["at"]
            tol = int(arg.get("tolerance", 1))
            self.record("assert_cursor", abs(x - ex) <= tol and abs(y - ey) <= tol,
                        f"cursor ({x},{y}) expected ({ex},{ey}) +/-{tol}"
                        + (" [game window centre]" if arg.get("window_centre") else ""))
        elif kind == "sample_victims":
            vs = self.resolver.victims()
            self.samples[f"victims.{arg}"] = [[n, round(x, 2), round(y, 2), round(z, 2), round(sx, 2), round(sz, 2)]
                                              for n, x, y, z, sx, sz in vs]
            self.record("sample_victims", len(vs) > 0, f"{arg}: {len(vs)} victim(s)")
        elif kind == "assert_victims_equal":
            runs = [self.samples.get(f"victims.{l}") for l in arg["runs"]]
            tol = float(arg.get("tolerance", 0.5))
            ok = all(runs) and all(len(r) == len(runs[0]) for r in runs)
            if ok:
                for r in runs[1:]:
                    for a, b in zip(runs[0], r):
                        ok = ok and a[0] == b[0] and all(abs(a[i] - b[i]) <= tol for i in (1, 2, 3))
            self.record("assert_victims_equal", ok,
                        f"{arg['runs']}: positions {'identical' if ok else 'DIFFER'} (+/-{tol} u): {runs}")
        elif kind == "assert_victims_at_spot0":
            vs = self.samples.get(f"victims.{arg}") or []
            ok = bool(vs) and all(abs(v[1] - v[4]) <= 0.5 and abs(v[3] - v[5]) <= 0.5 for v in vs)
            self.record("assert_victims_at_spot0", ok,
                        f"{arg}: every victim at its spot 0 (x,z; y is floor-snapped): {vs}")
        elif kind == "sample_pipe":
            # {cmd, as}: store a "k=v k=v" pipe reply as a dict
            reply = pipe.call(arg["cmd"])
            self.samples[arg["as"]] = dict(kv.split("=", 1) for kv in reply.split() if "=" in kv)
        elif kind == "assert_sample_ratio":
            # {num: [from, to, key], den: [from, to, key], min, max}: the change of a sampled
            # numeric field between two sample_pipe labels, divided by another such change.
            # Measures a cost per frame (us_total / frames over one window) or one window
            # against another (frames shown / frames hidden), so the display's refresh rate
            # cancels out (D46).
            def delta(spec):
                a, b, key = spec
                return int(self.samples[b][key]) - int(self.samples[a][key])
            n, d = delta(arg["num"]), delta(arg["den"])
            ratio = n / d if d else float("inf")
            ok = d > 0 and ("min" not in arg or ratio >= float(arg["min"])) and \
                ("max" not in arg or ratio <= float(arg["max"]))
            want = " and ".join(([f">= {arg['min']}"] if "min" in arg else []) + ([f"<= {arg['max']}"] if "max" in arg else []))
            self.record(kind, ok, f"{arg['num'][2]} {arg['num'][0]}->{arg['num'][1]} {n:+d} / "
                                  f"{arg['den'][2]} {arg['den'][0]}->{arg['den'][1]} {d:+d} = {ratio:.3f}, want {want}")
        elif kind == "assert_pipe_contains":
            # {cmd, contains | absent}: text of a pipe reply
            reply = pipe.call(arg["cmd"])
            if "contains" in arg:
                self.record(kind, arg["contains"] in reply, f"{arg['cmd']} contains {arg['contains']!r}: {reply[:200]!r}")
            else:
                self.record(kind, arg["absent"] not in reply, f"{arg['cmd']} lacks {arg['absent']!r}: {reply[:200]!r}")
        elif kind == "assert_pipe_stat":
            # {cmd, key, min|max|equals|changed, since: <sample label>} numeric field of a pipe
            # reply (min and max may be combined; changed: true = differs from the sample)
            reply = dict(kv.split("=", 1) for kv in pipe.call(arg["cmd"]).split() if "=" in kv)
            since = self.samples.get(arg["since"], {}) if arg.get("since") else {arg["key"]: "0"}
            if arg["key"] not in reply or arg["key"] not in since:
                # A field the core no longer sends would read as a number and could pass a
                # min/max check forever: a missing field is a failure.
                self.record("assert_pipe_stat", False, f"{arg['cmd']}.{arg['key']} missing"
                            + (f" (reply or sample {arg['since']!r})" if arg.get("since") else f": {reply}"))
                return
            val = int(reply[arg["key"]])
            base = int(since[arg["key"]])
            d = val - base
            if arg.get("changed"):
                ok, want = d != 0, "changed"
            elif "equals" in arg:
                ok, want = d == int(arg["equals"]), f"== {arg['equals']}"
            else:
                ok = ("min" not in arg or d >= int(arg["min"])) and ("max" not in arg or d <= int(arg["max"]))
                want = " and ".join(([f">= {arg['min']}"] if "min" in arg else []) + ([f"<= {arg['max']}"] if "max" in arg else []))
            self.record("assert_pipe_stat", ok,
                        f"{arg['cmd']}.{arg['key']} = {val}" + (f" ({d:+d} since {arg['since']})" if arg.get("since") else "")
                        + f", want {want}")
        elif kind == "assert_hud_matches":
            self.assert_hud(arg if isinstance(arg, dict) else {})
        elif kind == "assert_snapshot_matches":
            self.assert_snapshot(arg if isinstance(arg, dict) else {})
        elif kind == "run_setup":
            with open(os.path.join(SETUPS_DIR, f"{arg}.yaml"), encoding="utf-8") as fh:
                for sub in yaml.safe_load(fh)["steps"]:
                    if self.g.check_crash():
                        return
                    self.step(sub)
        elif kind == "assert_file":
            path = os.path.join(self.game_dir, arg["path"])
            try:
                with open(path, encoding="utf-8", errors="replace") as fh:
                    text = fh.read()
            except OSError:
                text = None
            if "contains" in arg:
                self.record("assert_file", text is not None and arg["contains"] in text,
                            f"{arg['path']} contains {arg['contains']!r}" + ("" if text is not None else " (missing)"))
            if "absent" in arg:
                self.record("assert_file", text is None or arg["absent"] not in text,
                            f"{arg['path']} does not contain {arg['absent']!r}")
        elif kind == "assert_registry":
            self.assert_registry(arg if isinstance(arg, dict) else {})
        elif kind == "assert_recordings":
            self.assert_recordings(arg)
        elif kind == "assert_log_count":
            n = self.read_core_log().count(arg["contains"])
            self.record("assert_log_count", n == int(arg["equals"]),
                        f"{arg['contains']!r} occurs {n} time(s), expected {arg['equals']}")
        elif kind == "assert_log":
            text = self.read_core_log()
            needle = arg["contains"].replace("{core_version}", core_version())
            self.record("assert_log", needle in text, f"contains {needle!r}")
        elif kind == "loadmap":
            # {loadmap: bank}: load a map and wait until it's in play (replaces console + wait_s 40)
            crash = self.load_map(str(arg))
            self.record("loadmap", crash is None, crash or f"{arg} loaded")
        elif kind == "console":
            self.console_log_text = self.read_core_log()  # for wait_log {since_console}
            inputs.type_text("`")  # backtick opens the game console in typing mode (R3b)
            time.sleep(0.2)
            inputs.type_text(str(arg).replace("{repo}", REPO_DIR.replace("\\", "/")))  # / types; \ has no scan code here
            inputs.tap("enter")
            time.sleep(0.1)
            inputs.type_text("`")
        else:
            raise ValueError(f"unknown step {kind}")

    def assert_recordings(self, opt):
        """Validate this test's actual recorder output, identified by its startup log."""
        def require(ok, message):
            if not ok:
                raise ValueError(message)

        def vector(value):
            return (isinstance(value, list) and len(value) == 3
                    and all(isinstance(v, (int, float)) and math.isfinite(v) for v in value))

        try:
            paths = list(dict.fromkeys(line.split("recorder: started ", 1)[1].strip()
                         for line in self.read_core_log().splitlines() if "recorder: started " in line))
            runs, victim_samples = 0, 0
            for path in paths:
                with open(path, encoding="utf-8") as stream:
                    rows = [json.loads(line) for line in stream if line.strip()]
                require(bool(rows), f"{path}: empty recording")
                header = rows[0]
                require(header.get("type") == "start" and header.get("schema") == "fdny-trajectory-v1",
                        f"{path}: invalid header")
                if opt.get("stage") and header.get("stage") != opt["stage"]:
                    continue
                samples, last_time, lifetimes = 0, {}, {}
                for row in rows[1:-1]:
                    kind = row.get("type")
                    require(kind in ("sample", "victim_sample", "victim_end"), f"{path}: unexpected record")
                    track = "player" if kind == "sample" else row.get("id")
                    require(isinstance(track, str) and bool(track), f"{path}: missing track ID")
                    t = row.get("t_ms")
                    require(isinstance(t, (int, float)) and math.isfinite(t) and t >= last_time.get(track, 0),
                            f"{path}: invalid or out-of-order time")
                    last_time[track] = t
                    if kind == "victim_end":
                        require(lifetimes.get(track) == "alive", f"{path}: invalid victim end")
                        lifetimes[track] = "ended"
                        continue
                    require(vector(row.get("position")) and vector(row.get("body_forward")),
                            f"{path}: invalid position or facing")
                    lo, hi = row.get("bounds_min"), row.get("bounds_max")
                    require(vector(lo) and vector(hi) and all(a < b for a, b in zip(lo, hi)),
                            f"{path}: invalid collision bounds")
                    if kind == "sample":
                        require(row.get("n") == samples and vector(row.get("velocity")),
                                f"{path}: invalid sample sequence or velocity")
                        samples += 1
                    else:
                        require(lifetimes.get(track) != "ended", f"{path}: victim ID reused after destruction")
                        lifetimes[track] = "alive"
                        victim_samples += 1
                require(rows[-1].get("type") == "end" and rows[-1].get("samples") == samples,
                        f"{path}: invalid closing record")
                require(samples >= opt.get("min_samples", 1), f"{path}: too few samples")
                shutil.copy2(path, os.path.join(self.out, os.path.basename(path)))
                runs += 1
            require(runs >= opt.get("min_runs", 1), "too few completed recordings")
            require(not opt.get("victims") or victim_samples > 0, "no victim samples")
            self.record("assert_recordings", True, f"{runs} valid recordings, {victim_samples} victim samples")
        except (OSError, ValueError, TypeError, KeyError, AttributeError) as error:
            self.record("assert_recordings", False, str(error))

    def assert_registry(self, opt):
        """D41: what the build registered (fd_dbg_registry's file) == what tools/docgen.py reads
        from the source, per kind; Debug-only items included (this is the Debug build)."""
        name = opt.get("file", "FDNYCore_registry.txt")
        src = os.path.join(self.game_dir, name)
        try:
            with open(src, encoding="utf-8", errors="replace") as fh:
                rows = [l.rstrip("\r\n").split("\t") for l in fh if l.strip()]
        except OSError:
            self.record("assert_registry", False, f"{name} missing (fd_dbg_registry not run?)")
            return
        shutil.copy2(src, os.path.join(self.out, name))
        sys.path.insert(0, os.path.join(REPO_DIR, "tools"))
        import docgen
        reg = docgen.read_source()
        # A module that disabled itself (window, on a signature mismatch) registers nothing: its
        # items are expected only when it is enabled. Every module must be known to both.
        mods = {r[1]: r[2] == "1" for r in rows if r[0] == "module"}
        src_mods = {s["module"]: s["file"] for s in reg["sections"] if s["module"]}
        self.record("assert_registry", set(src_mods) <= set(mods),
                    f"modules: {len(mods)} in the build, {sorted(src_mods)} read from the source"
                    + (f"; disabled in this run: {sorted(m for m, on in mods.items() if not on)}"
                       if not all(mods.values()) else ""))
        off = {f for m, f in src_mods.items() if not mods.get(m, True)}
        for k in ("cvars", "commands", "binds", "keys"):
            reg[k] = [x for x in reg[k] if x["file"] not in off]
        b = lambda x: "1" if x else "0"
        want = {
            "cvar": {(c["name"], c["default"], b(c["read_only"]), b(c["archive"]), b(c["practice"]), c["help"])
                     for c in reg["cvars"]},
            "cmd": {(c["name"], b(c["practice"]), c["help"]) for c in reg["commands"]},
            "bind": {(x["key"], x["line"]) for x in reg["binds"]},
            "key": {(k["key"], k["action"], k["when"]) for k in reg["keys"]},
        }
        for kind, expected in want.items():
            got = {tuple(r[1:]) for r in rows if r[0] == kind}
            only_build, only_src = sorted(got - expected), sorted(expected - got)
            self.record("assert_registry", not only_build and not only_src,
                        f"{kind}: {len(got)} in the build, {len(expected)} read from the source"
                        + (f"; only in the build: {only_build}" if only_build else "")
                        + (f"; only in the source: {only_src}" if only_src else ""))

    def read_full_log(self):
        try:
            with open(self.g.core_log, encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def read_core_log(self):
        """FDNYCore.log as this test sees it. In a shared session: the session's startup, then
        only this test's lines, so an assertion can't pass on another test's output."""
        text = self.read_full_log()
        if self.session and self.log_from:
            return text[:self.session.startup_end] + text[self.log_from:]
        return text

    def assert_frame_hooks(self, seconds, tolerance):
        """Independent frame count: g_gameClock's published elapsed-ms changes
        once per GameSession::RunFrame, and in a level each RunFrame renders
        one BeginFrame/EndFrame pair. Poll it fast and count changes, then
        compare with the core's hook counters over the same window."""
        s0 = pipe.stats()
        last, ticks = self.resolver.read("clock.elapsed_ms"), 0
        t_end = time.perf_counter() + seconds
        while time.perf_counter() < t_end:
            v = self.resolver.read("clock.elapsed_ms")
            if v != last:
                ticks += 1
                last = v
        s1 = pipe.stats()
        db, de, dv = (s1[k] - s0[k] for k in ("begin", "end", "events"))
        self.samples.setdefault("frame_windows", []).append(
            {"seconds": seconds, "clock_ticks": ticks, "begin": db, "end": de, "events": dv})
        self.record("frame_hooks_once_per_frame",
                    ticks > 0 and abs(db - ticks) <= tolerance and abs(de - ticks) <= tolerance
                    and abs(db - de) <= 1 and abs(dv - de) <= 1,
                    f"{seconds}s: clock ticks={ticks} BeginFrame={db} EndFrame={de} frame_end events={dv} "
                    f"(tolerance {tolerance} for window-edge races)")

    def assert_juggle(self, opt):
        """fd_juggle's arrow against the harness's own computation: player position, yaw
        and the safe spot (+0x3400) read directly from memory; landing point T = spot -
        30 X; view axes measured live (forward = (sin yaw, cos yaw), right = (-cos yaw,
        sin yaw), docs/evidence/fire_juggle_overlay); 8 sectors of 45 degrees,
        8 = "o" under 16 u. {locked: 0|1} (default 1) is asserted too; {locked: auto} derives
        it from memory for a player who isn't moving (frozen = spot != position, exactly,
        since Think's write copies the position), and fails if the position changes
        between two reads."""
        reply = dict(kv.split("=", 1) for kv in pipe.call("juggle").split() if "=" in kv)
        r = self.resolver
        pl = self.g.pm.read_uint(r.address("player.object"))
        f32 = lambda off, i=0: struct.unpack("<f", self.g.pm.read_bytes(pl + off + 4 * i, 4))[0]
        pos = [f32(r.field("player.f_pos"), i) for i in range(3)]
        safe = [f32(r.field("player.f_safe"), i) for i in range(3)]
        yaw = f32(r.field("player.f_yaw"))
        if opt.get("locked") == "auto":
            # wait (up to 2 s) for two equal position reads 0.1 s apart: a teleported player
            # settles onto the floor first
            t0 = time.time()
            while True:
                time.sleep(0.1)
                pos2 = [f32(r.field("player.f_pos"), i) for i in range(3)]
                if pos2 == pos or time.time() - t0 > 2:
                    break
                pos = pos2
            if pos2 != pos:
                self.record("assert_juggle", False, f"locked: auto needs a still player: pos {pos} -> {pos2}")
                return
            reply = dict(kv.split("=", 1) for kv in pipe.call("juggle").split() if "=" in kv)
            safe = [f32(r.field("player.f_safe"), i) for i in range(3)]
            yaw = f32(r.field("player.f_yaw"))
            opt = dict(opt, locked=0 if safe == pos else 1)
        tx, tz = safe[0] - 30.0, safe[2]
        dx, dz = tx - pos[0], tz - pos[2]
        dist = math.hypot(dx, dz)
        s, c = math.sin(math.radians(yaw)), math.cos(math.radians(yaw))
        fwd, right = dx * s + dz * c, -dx * c + dz * s
        ang = math.degrees(math.atan2(right, fwd)) % 360
        want = {8} if dist < 16 else {int(((ang + 22.5) % 360) // 45)}
        edge = (ang + 22.5) % 45
        if dist >= 16 and (edge < 1 or edge > 44):  # within 1 degree of a sector edge: either side
            want |= {int(((ang + 22.5 + (2 if edge > 44 else -2)) % 360) // 45)}
        locked = int(opt.get("locked", 1))
        got_dir, got_dist = int(reply.get("dir", "-9")), int(reply.get("dist", "-1"))
        tgt = [int(v) for v in reply.get("target", "0,0,0").split(",")]
        ok = (int(reply.get("locked", "-1")) == locked and (got_dir in want or not locked)
              and abs(got_dist - dist * 10) <= 10 and abs(tgt[0] - tx * 10) <= 1 and abs(tgt[2] - tz * 10) <= 1)
        self.record("assert_juggle", ok,
                    f"pipe locked={reply.get('locked')} dir={got_dir} dist={got_dist / 10:.1f} target={tgt}; harness: "
                    f"pos=({pos[0]:.1f},{pos[2]:.1f}) spot=({safe[0]:.1f},{safe[2]:.1f}) yaw={yaw:.1f} -> "
                    f"angle {ang:.1f} dir {sorted(want)} dist {dist:.1f}; want locked={locked}")

    def assert_snapshot(self, opt):
        """Item 9: the core's per-frame snapshot (Debug pipe) vs. the harness's
        own direct memory reads, taken immediately after. Exact for fields that
        don't change frame to frame while the player is idle; time_left within
        `time_tol` (it counts down) and motion fields within `motion_tol`."""
        snap = pipe.snapshot()
        r = self.resolver
        time_tol, motion_tol = float(opt.get("time_tol", 0.5)), float(opt.get("motion_tol", 1.0))
        fails, n = [], 0

        def cmp(key, direct, tol=0.0):
            nonlocal n
            n += 1
            got = snap.get(key)
            if got is None or direct is None:
                fails.append(f"{key}: snapshot={got} direct={direct}")
                return
            if isinstance(direct, str):
                ok = got == (direct or "-")
            else:
                ok = abs(float(got) - float(direct)) <= tol + 1e-3
            if not ok:
                fails.append(f"{key}: snapshot={got} direct={direct} (tol {tol})")

        cmp("game_state", r.read("game.state"))
        cmp("map", r.read("game.map_name"))
        cmp("menu", r.menu_screen())
        for k, name, tol in (("pos_x", "player.pos_x", motion_tol), ("pos_y", "player.pos_y", motion_tol),
                             ("pos_z", "player.pos_z", motion_tol), ("vel_x", "player.vel_x", motion_tol),
                             ("vel_y", "player.vel_y", motion_tol), ("vel_z", "player.vel_z", motion_tol),
                             ("pitch", "player.pitch", motion_tol), ("yaw", "player.yaw", motion_tol),
                             ("time_left", "mission.time_left", time_tol), ("move_state", "player.state", 0),
                             ("victims_total", "victims.total", 0), ("victims_rescued", "victims.rescued", 0),
                             ("bot_enabled", "rescuebot.enabled", 0), ("bot_claimed", "rescuebot.claimed", 0)):
            cmp(k, r.read(name), tol)
        vx, vz = r.read("player.vel_x") or 0.0, r.read("player.vel_z") or 0.0
        cmp("speed_xy", (vx * vx + vz * vz) ** 0.5, motion_tol)
        vs = r.victims()
        cmp("victims", len(vs))
        for i, (_, x, y, z, _, _) in enumerate(vs):
            got = [float(c) for c in snap.get(f"v{i}", "nan,nan,nan,0").split(",")[:3]]
            n += 1
            if any(abs(a - b) > motion_tol for a, b in zip(got, (x, y, z))):
                fails.append(f"v{i}: snapshot={got} direct={(x, y, z)}")
        total, burning, out = r.fires()
        cmp("fires", total)
        cmp("fires_burning", burning)
        cmp("fires_out", out)
        self.samples.setdefault("snapshots", []).append(snap)
        self.record("assert_snapshot_matches", not fails,
                    f"{n} fields compared at frame {snap.get('frame')}; mismatches: {fails or 'none'}")

    def assert_hud(self, opt):
        """Item 10, two links:
        (1) same frame: every number the HUD drew equals the snapshot value it
            was drawn from (1-decimal rounding: +/-0.051) -- holds while moving;
        (2) at rest: the drawn values equal direct memory reads (the snapshot
            == memory link is item 9's guarantee). Call with the player idle."""
        raw = pipe.call("hud")
        parts = raw.split("|")
        hud_frame = int(parts[0].split("=", 1)[1]) if parts and parts[0].startswith("frame=") else -1
        lines = {p.split(" ", 1)[0]: p.split(" ", 1)[1].strip() if " " in p else "" for p in parts[1:]}
        r = self.resolver
        fails = []
        if not lines:
            fails.append(f"no HUD lines drawn ({raw!r})")
        src = [float(t) for t in lines.get("src", "").split()]
        drawn = []
        for key, n in (("pos", 3), ("vel", 3), ("speed", 1)):
            try:
                drawn += [float(t) for t in lines.get(key, "").split()[:n]]
            except ValueError:
                pass
        if len(src) < 10 or len(drawn) != 7:
            fails.append(f"same-frame check: drawn {drawn} / src {src} incomplete")
        else:
            bad = [(d, s) for d, s in zip(drawn, src[:7]) if abs(d - s) > 0.051]
            if bad:
                fails.append(f"same-frame drawn != source: {bad}")
            # stance (owner 2026-10-23): crouched = state 4; bugged = crouchbug (not 4, box top < 55)
            want = ("crouched" if int(src[7]) == 4 else
                    "bugged" if len(src) > 10 and src[10] < 55 else "standing")
            if lines.get("state", "").strip() != want:
                fails.append(f"same-frame state {lines.get('state')!r} vs source state {int(src[7])} -> {want}")
        if opt.get("moving"):
            # mid-motion: only the same-frame link is meaningful (and must hold)
            if len(src) >= 7 and abs(src[6]) < 1.0:
                fails.append(f"expected motion but source speed is {src[6]}")
            self.samples.setdefault("hud", []).append(raw)
            self.record("assert_hud_matches", not fails,
                        f"[moving] HUD frame {hud_frame}: {lines}; mismatches: {fails or 'none'}")
            return
        # (2) at rest vs direct memory
        vel = [r.read(f"player.vel_{c}") or 0.0 for c in "xyz"]
        tol = 0.06

        def nums(key, n):
            try:
                return [float(t) for t in lines.get(key, "").split()[:n]]
            except ValueError:
                return []

        pos = nums("pos", 3)
        direct_pos = [r.read(f"player.pos_{c}") for c in "xyz"]
        if len(pos) != 3 or any(abs(a - b) > tol for a, b in zip(pos, direct_pos)):
            fails.append(f"pos drawn {pos} vs memory {direct_pos} (tol {tol:.2f})")
        v = nums("vel", 3)
        if len(v) != 3 or any(abs(a - b) > 0.06 + abs(b) * 0.1 for a, b in zip(v, vel)):
            fails.append(f"vel drawn {v} vs memory {vel}")
        sp = nums("speed", 1)
        direct_sp = (vel[0] ** 2 + vel[2] ** 2) ** 0.5
        if len(sp) != 1 or abs(sp[0] - direct_sp) > 0.06 + direct_sp * 0.1:
            fails.append(f"speed drawn {sp} vs memory {direct_sp:.2f}")
        # stance from memory: move state + the collision box top (player.f_maxs y)
        st = lines.get("state", "").strip()
        mstate = r.read("player.state")
        pl = self.g.pm.read_uint(r.address("player.object"))
        top = struct.unpack("<f", self.g.pm.read_bytes(pl + r.field("player.f_maxs") + 4, 4))[0]
        want = "crouched" if mstate == 4 else ("bugged" if top < 55 else "standing")
        if st != want:
            fails.append(f"state drawn {st!r} vs memory move_state {mstate}, box top {top:.0f} -> {want}")
        if "screen" in lines:  # removed (owner 2026-10-23)
            fails.append(f"a screen line is still drawn: {lines['screen']!r}")
        self.samples.setdefault("hud", []).append(raw)
        self.record("assert_hud_matches", not fails,
                    f"HUD frame {hud_frame}: {lines}; mismatches: {fails or 'none'}")

    def assert_value(self, a, val):
        name, tol = a["name"], float(a.get("tolerance", 0))
        if val is None:
            return self.record("assert_mem", False, f"{name} unresolved (null pointer in chain)")
        if "equals" in a or "equals_sample" in a:
            exp = self.samples[a["equals_sample"]] if "equals_sample" in a else a["equals"]
            ok = abs(val - exp) <= tol if isinstance(val, (int, float)) and not isinstance(exp, str) else val == exp
            return self.record("assert_mem", ok, f"{name}={val!r} expected {exp!r} +/-{tol}")
        if "not_equals" in a:
            return self.record("assert_mem", val != a["not_equals"], f"{name}={val!r} != {a['not_equals']!r}")
        ok = ("min" not in a or val >= a["min"]) and ("max" not in a or val <= a["max"])
        return self.record("assert_mem", ok, f"{name}={val!r} in [{a.get('min')}, {a.get('max')}]")

    # --- lifecycle ---------------------------------------------------------
    def setup_steps(self):
        setup = self.test.get("setup")
        if not setup:
            return []
        with open(os.path.join(SETUPS_DIR, f"{setup}.yaml"), encoding="utf-8") as f:
            return yaml.safe_load(f)["steps"]

    def steps_with_setup(self):
        return self.setup_steps() + self.test.get("steps", [])

    def run_steps(self, steps):
        for s in steps:
            crash = self.g.check_crash()
            if crash:
                return crash
            self.step(s)
        return self.g.check_crash()

    def launch(self):
        """Start the game (the only place a test launches it) and attach. Returns a crash or None."""
        self.g.launch(self.test.get("env"))
        crash = self.g.wait_window()
        if not crash:
            # Screenshots/video grab the screen region, and input goes to the
            # foreground window: the game must be on top.
            time.sleep(1.0)
            focused = inputs.focus(self.g.hwnd)
            self.record("focus", focused, "game window foreground at launch")
            if not focused:
                # Every later step would type/click into another window: stop here.
                return "harness: could not focus the game window (Windows foreground lock)"
        if not crash and self.core_config:
            crash = self.g.wait_ready()
            self.record("core_ready", crash is None, crash or "ready line seen")
        if crash:
            return crash
        bases = self.g.attach()
        self.samples["module.exe"] = hex(bases.get("exe", 0))
        self.samples["module.genesis"] = hex(bases.get("genesis", 0))
        exe_sha = game.sha256(self.g.exe)
        gen_sha = game.sha256(os.path.join(self.game_dir, "Genesis.dll"))
        vname, version = addrtable.select_version(addrtable.load(), exe_sha, gen_sha)
        self.record("address_table_version", version is not None, vname or "no matching exe+Genesis hash")
        if version:
            self.resolver = addrtable.Resolver(self.g.pm, version, bases)
            self.resolver_version = version
        return None

    def join_session(self, first=False):
        """Start this test in the session's running game: the core reset to its defaults, the
        test's FDNYCore.cfg (as fdtest_core.cfg) exec'd, Training reloaded. The session's first
        test starts in the level its setup just loaded (defaults already), so it reloads only
        to apply a cfg. From here on the test sees only its own FDNYCore.log lines (plus the
        session's startup)."""
        if not inputs.focus(self.g.hwnd):
            return "harness: could not focus the game window (Windows foreground lock)"
        self.log_from = len(self.read_full_log())
        has_cfg = any(f["dst"].lower() == "fdnycore.cfg" and f.get("src") for f in self.test.get("pre_files", []))
        if first and not has_cfg:
            return None
        if not first:
            with open(os.path.join(self.game_dir, session.RESET_CFG), "w", encoding="utf-8", newline="\r\n") as f:
                f.write("\n".join(session.reset_lines(REPO_DIR)) + "\n")
            self.step({"console": f"exec {session.RESET_CFG}"})
        if has_cfg:
            self.step({"console": f"exec {session.TEST_CFG}"})
        t0 = time.time()
        crash = self.load_map("training")
        if not crash:
            self.record("session", True, f"reset, cfg applied, Training reloaded in {time.time() - t0:.1f} s")
        return crash

    def load_map(self, name):
        """`loadmap <name>`, then wait for the core's level_load event for it (not a fixed time),
        plus 3 s for victims to spawn and the first frames to settle."""
        event = f"event: level_load '{name}'"
        loads = self.read_full_log().count(event)
        self.step({"console": f"loadmap {name}"})
        t0 = time.time()
        while self.read_full_log().count(event) <= loads:
            crash = self.g.check_crash()
            if crash:
                return crash
            if time.time() - t0 > 90:
                return f"harness: {name} didn't load within 90 s"
            time.sleep(0.5)
        time.sleep(3.0)
        return None

    def place_pre_files(self, shared=False):
        """pre_files: [{src: <path rel. to core/>, dst: <name in game dir>}] copied
        before launch and removed after the run (e.g. Debug address-table overrides).
        shared: in a session, only the files this test reads while it runs; the load-time
        ones belong to the session, and FDNYCore.cfg is exec'd as fdtest_core.cfg."""
        placed = []
        for f in self.test.get("pre_files", []):
            if shared and session.is_load_time(f["dst"]):
                continue
            if shared and f["dst"].lower() == "fdnycore.cfg":
                f = {"dst": session.TEST_CFG, "src": f.get("src")}
            dst = os.path.join(self.game_dir, f["dst"])
            # Per run: baseline_route's runs without the core are nested in its own run, and a
            # shared name let the inner run consume the outer backup, so the outer run then
            # deleted the game's FDNYFireFighter.cfg (2026-10-26).
            backup = f"{dst}.fdh-backup-{id(self):x}"
            if os.path.exists(dst):  # a real game file: restore it afterwards
                shutil.copy2(dst, backup)
            if f.get("src"):
                os.makedirs(os.path.dirname(dst), exist_ok=True)  # e.g. a recordings folder
                shutil.copy2(os.path.join(REPO_DIR, f["src"]), dst)
            elif os.path.exists(dst):  # src: null -> the file must be absent for the test
                os.remove(dst)
            placed.append((dst, backup))
        return placed

    def snapshot_saves(self):
        """The career saves (Saves/player*.rb). Winning a level writes progress into the
        slot the setups use ("Hotrod Stein", at Training): every later test would then start
        a different mission (2026-10-23: the autosplitter probes moved it on). Kept in
        memory and put back after every test."""
        saves = {}
        d = os.path.join(self.game_dir, "Saves")
        for name in (os.listdir(d) if os.path.isdir(d) else []):
            with open(os.path.join(d, name), "rb") as fh:
                saves[name] = fh.read()
        return saves

    def restore_saves(self, saves):
        d = os.path.join(self.game_dir, "Saves")
        changed = []
        for name, data in saves.items():
            p = os.path.join(d, name)
            try:
                with open(p, "rb") as fh:
                    if fh.read() == data:
                        continue
            except OSError:
                pass
            with open(p, "wb") as fh:
                fh.write(data)
            changed.append(name)
        if changed:
            print(f"  (career saves the game rewrote, restored: {', '.join(changed)})")

    def execute(self):
        s = self.session
        if s and not s.alive:  # first test of a session (or after a crash): set up its launch
            s.close()
            deploy_core(self.game_dir, self.core_config)
            s.place(self.game_dir, self.test.get("pre_files", []), REPO_DIR)
        elif not s:
            deploy_core(self.game_dir, self.core_config)
        placed = self.place_pre_files(shared=bool(s))
        saves = self.snapshot_saves()
        try:
            return self._execute()
        finally:
            self.restore_saves(saves)
            for dst, backup in placed:
                try:
                    if os.path.exists(backup):
                        os.replace(backup, dst)
                    else:
                        os.remove(dst)
                except OSError:
                    pass

    def _execute(self):
        expect = self.test.get("expect", {})
        crash = None
        rec = None
        s = self.session
        print(f"== {self.test['name']} (core={self.core_config}"
              + (", shared session" if s and s.alive else ", new session" if s else "") + f") -> {self.out}")
        try:
            if s and s.alive:
                self.g, self.resolver, self.resolver_version = s.g, s.resolver, s.version
                steps = self.test.get("steps", [])
                crash = self.join_session()
            elif s:
                # A new session: launch, its setup (shared by every test in it), then start
                # this test like every later one (reset, its cfg, Training reloaded).
                steps = self.test.get("steps", [])
                crash = self.launch() or self.run_steps(self.setup_steps())
                if not crash:
                    s.g, s.resolver, s.version = self.g, self.resolver, self.resolver_version
                    s.startup_end = len(self.read_full_log())
                    crash = self.join_session(first=True)
            else:
                steps = self.steps_with_setup()
                crash = self.launch()
            if not crash:
                if self.test.get("record", True):
                    rec = capture.Recorder(os.path.join(self.out, "run.mp4"), self.g.client_rect())
                    err = rec.start()
                    if err:
                        self.record("record", False, err)
                        rec = None
                crash = self.run_steps(steps)
        except Exception as e:  # harness bug or unreadable memory: fail the run, keep evidence
            crash = crash or f"harness exception: {e!r}"
        finally:
            if rec:
                rec.stop()
            try:
                if self.g.hwnd and not crash:
                    capture.screenshot(self.g.client_rect(), os.path.join(self.out, "final.png"))
            except Exception:
                pass
            if not s:
                self.g.kill()
            elif crash:
                s.close()  # the next test of this session starts a new one

        if expect.get("no_crash", True):
            self.record("no_crash", crash is None, crash or "no crash, hang or dialog")
        if self.core_config:
            errors = self.collect_log()
            limit = expect.get("log_errors", 0)
            self.record("log_errors", len(errors) <= limit, f"{len(errors)} ERROR line(s), limit {limit}")
        passed = all(r["pass"] for r in self.results)
        result = {"test": self.test["name"], "core": self.core_config, "pass": passed,
                  "crash": crash, "assertions": self.results, "samples": self.samples,
                  "video": os.path.join(self.out, "run.mp4") if rec else None}
        if expect.get("baseline_equivalent") and self.core_config:
            result = self.check_baseline(result)
            passed = result["pass"]
        with open(os.path.join(self.out, "result.json"), "w", encoding="utf-8") as f:
            json.dump(result, f, indent=2)
        print(f"== {'PASS' if passed else 'FAIL'} {self.test['name']}")
        return result

    def check_baseline(self, result):
        """Item 1(c) "behaves the same", asserted (suite review item 3): this run (with the
        core) against two runs of the same test with the core removed, back to back on the same
        display, by tools/harness/compare_runs.py's checks (state, module bases, autosplitter chains,
        frame rate, lap positions, log errors)."""
        import sys as _sys
        _sys.path.insert(0, os.path.join(REPO_DIR, "tools", "harness"))
        import compare_runs
        if self.session:  # a second game exits at once (code 0) while the session's one runs (D48)
            self.session.close()
        bases = [Run(self.test_path, self.game_dir, None, f"{self.tag}-base{i}").execute() for i in (1, 2)]
        ok, checks = compare_runs.compare(bases[0], bases[1], result)
        failed = [f"{c['check']}: {c['detail']}" for c in checks if not c["pass"]]
        self.record("baseline_equivalent", ok,
                    f"{len(checks) - len(failed)}/{len(checks)} checks vs 2 runs without the core"
                    + (f"; failed: {failed}" if failed else ""))
        self.samples["baseline_checks"] = checks
        result["pass"] = all(r["pass"] for r in self.results)
        return result

    def collect_log(self):
        if not os.path.exists(self.g.core_log):
            return ["FDNYCore.log missing"]
        text = self.read_core_log()  # this test's part, in a shared session
        with open(os.path.join(self.out, "FDNYCore.log"), "w", encoding="utf-8") as f:
            f.write(text)
        return [l for l in text.splitlines() if re.search(r"\] \[ERROR\] ", l)]
