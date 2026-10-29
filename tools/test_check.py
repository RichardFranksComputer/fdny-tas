"""Keeps the tests and the code in step (D61). A rename in the code that a game test still
uses, a test nothing names, a fixture nobody reads: each fails here in seconds, not after a
game run or never.

    python -I tools/test_check.py                     # the checks (pre-commit hook, tests/unit, CI)
    python -I tools/test_check.py affected [<base>]   # the game tests a change touches
                                                      # (default base: origin/main)

What the game tests (tests/game, tools/harness/setups) name must exist in the code:
  - every step kind is one the harness runs (tools/harness/fdh/runner.py);
  - every pipe channel (`cmd:`) is registered (testpipe::Register, testpipe.cpp's own),
    and every field a step reads (`key:`) is written by some source line (`<key>=`);
  - every fd_ name in a step, a test cfg or a setup is a registered variable, command or
    old name; every `name:` of assert_mem / sample_mem is an address-table entry;
  - every expected text (assert_log, wait_log, assert_log_count, assert_pipe_contains,
    `contains:` or `absent:`) can be produced by a string literal in the source (printf
    fields match anything) or is text the test feeds in itself (an echo line);
    an `absent:` or `equals: 0` check on a string the code never prints passes forever;
  - every file a test copies or reads exists, and every file in tests/game/data is used.
Both directions of the map between code and tests:
  - every tests/game/*.yaml is named by a `// Tests:` line (core or feature), and every
    path a `// Tests:` line names exists: `affected` reads these lines;
  - every player-facing variable and command appears in some test (game or unit) or in
    UNTESTED below with the reason. An UNTESTED entry that is now tested, or gone, is an
    error too, so the list stays true.
"""
import json
import os
import re
import subprocess
import sys

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.join(REPO, "tools"))
import docgen  # noqa: E402

GAME = os.path.join(REPO, "tests", "game")
DATA = os.path.join(GAME, "data")
SETUPS = os.path.join(REPO, "tools", "harness", "setups")
UNIT = os.path.join(REPO, "tests", "unit")
RUNNER = os.path.join(REPO, "tools", "harness", "fdh", "runner.py")

# Player-facing names no test uses, each with the reason (owner-visible; keep it short).
UNTESTED = {
    "fd_checkpoints_distance": "editor tuning; checkpoints.yaml edits at the default",
    "fd_checkpoints_save": "retries a failed autosave; checkpoints.yaml checks the autosave",
    "fd_checkpoints_status": "prints the timestamp state and checkpoint count only",
    "fd_checkpoints_step": "editor tuning; checkpoints.yaml edits at the default",
    "fd_fov_factor": "fov_sets_camera measures its default (1.4, the classic fix); other values aren't",
    "fd_fovinfo": "prints the camera values only",
    "fd_freecam_sens": "flight tuning; freecam.yaml measures the default's turn rate",
    "fd_freecam_speed": "flight tuning; freecam.yaml measures the default's speed",
    "fd_replay_list": "prints the recordings only; recordings_menu lists them in the menu",
    "fd_spawns": "owner 2026-10-29: the default pins (spot 0, speedrun_static_spawns) are enough",
    "fd_text_alpha": "panel background opacity; text_alpha tests the text's (fd_text_fg_alpha)",
    "fd_text_size": "text height; text_panels checks the panels draw at the default",
    "fd_textdraw": "prints the panel list only",
    "fd_triggers_list": "prints the trigger list only",
    "fd_warp_freeze": "owner 2026-10-29: Z / X need no game test",
    "fd_window_cursor": "window_alttab tests its default (1) and the O key that frees the cursor",
}

LOG_PREFIXES = ("console> ", "[INFO] ", "[WARN] ", "[ERROR] ")
FMT = re.compile(r"%[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|I64)?[sdiuxXfgcpe]|%%")
QUOTED = r'"(?:[^"\\]|\\.)*"|\'(?:[^\']|\'\')*\'|[^,}\]\n#]+'
TEXT_KEY = re.compile(r"\b(contains|absent):\s*(" + QUOTED + ")")
WILD = None    # a printf field in a pattern
FIELD = "\0"   # in an expected text: a value a printf field prints (a registered name)
FEEDS = re.compile(r"^\s*- (console|type):\s*(" + QUOTED + ")", re.M)


def read(path):
    with open(path, encoding="utf-8-sig", errors="replace") as f:
        return f.read()


def rel(path):
    return os.path.relpath(path, REPO).replace("\\", "/")


def unquote(s):
    s = s.strip()
    if s.startswith('"'):
        return re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t"}.get(m.group(1), m.group(1)), s[1:-1])
    if s.startswith("'"):
        return s[1:-1].replace("''", "'")
    return s


def source_files():
    out = []
    for top in ("core", "features"):
        for root, _, files in os.walk(os.path.join(REPO, top)):
            if os.sep + "gen" in root:
                continue
            out += [os.path.join(root, f) for f in sorted(files) if f.endswith((".cpp", ".h"))]
    return sorted(out)


def test_files():
    tests = sorted(os.path.join(GAME, f) for f in os.listdir(GAME) if f.endswith(".yaml"))
    for sub in ("ondemand", "probes"):
        d = os.path.join(GAME, sub)
        if os.path.isdir(d):
            tests += sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith(".yaml"))
    setups = sorted(os.path.join(SETUPS, f) for f in os.listdir(SETUPS) if f.endswith(".yaml"))
    return tests, setups


class Code:
    """What the source offers the tests."""

    def __init__(self):
        self.files = {rel(p): read(p) for p in source_files()}
        self.raw = "\n".join(self.files.values())
        reg = docgen.read_source()
        self.reg = reg
        self.names = ({c["name"] for c in reg["cvars"]} | {c["name"] for c in reg["commands"]}
                      | {a["old"] for a in reg["aliases"]})
        self.public = sorted(c["name"] for c in reg["cvars"] + reg["commands"]
                             if not c["debug"] and not c["help"].startswith("internal:"))
        self.channels = {}  # channel -> file
        for f, text in self.files.items():
            for m in re.finditer(r'testpipe::Register\("(\w+)"', text):
                self.channels[m.group(1)] = f
            if f.endswith("testpipe.cpp"):
                for m in re.finditer(r'StartsWith\(req, "(\w+)', text):
                    self.channels[m.group(1)] = f
        table = json.loads(read(os.path.join(REPO, "core", "addresses.json")))
        self.entries = {n for v in table["versions"].values() for n in v["entries"]}
        self.steps = set(re.findall(r'kind == "(\w+)"', read(RUNNER)))
        for group in re.findall(r"kind in \(([^)]*)\)", read(RUNNER)):
            self.steps |= set(re.findall(r'"(\w+)"', group))
        # String literals as patterns: (file, [chars and WILD]); indexed by 4-grams.
        self.patterns, self.index, self.short, lits = [], {}, set(), []
        for f, text in self.files.items():
            for kind, val, _, _ in docgen.tokens(text):
                if kind != "str" or len(val) < 2:
                    continue
                lits.append(val)
                pieces = FMT.split(val)
                pat = []
                for i, piece in enumerate(pieces):
                    if i:
                        pat.append(WILD)
                    pat += list(piece)
                k = len(self.patterns)
                self.patterns.append((f, pat))
                for piece in pieces:
                    for g in range(len(piece) - 3):
                        self.index.setdefault(piece[g:g + 4], set()).add(k)
                if len(pieces) > 1 and max(len(p) for p in pieces) < 4:
                    self.short.add(k)  # "%s \"%s\"%s": no 4 characters to index it by
        # Literals a format can be handed whole through %s ("off", "level change").
        self.args = sorted({v.strip() for v in lits if "%" not in v and len(v) <= 40
                            and re.search(r"[A-Za-z]", v)}, key=len, reverse=True)

    def values(self, s, fed=""):
        """How many characters of s are spaces or may have been printed by a field: registered names,
        numbers and words with digits (ids, file names), other literals passed whole, words
        the test typed itself, {core_version}."""
        free = [c == FIELD for c in s]

        def mark(a, b):
            for i in range(a, b):
                free[i] = True
        for m in re.finditer(r"[\w.-]+", s):
            w = m.group(0)
            if w in self.names or w.startswith("fd_") or re.search(r"\d", w) or (fed and w in fed):
                mark(m.start(), m.end())
        for a in self.args:
            if a in s:
                pre = r"(?<!\w)" if re.match(r"\w", a) else ""
                post = r"(?!\w)" if re.search(r"\w$", a) else ""
                for m in re.finditer(pre + re.escape(a) + post, s):
                    mark(m.start(), m.end())
        return sum(1 for c, f in zip(s, free) if f or c.isspace())

    def matches(self, s, fed=""):
        """The literals (formats) that can print text containing s, as (file, pattern). Their
        fields match anything, so the characters of s that aren't field values must mostly
        be the format's own."""
        grams = {s[g:g + 4] for g in range(len(s) - 3) if FIELD not in s[g:g + 4]}
        cand = set().union(self.short, *(self.index.get(g, ()) for g in grams)) if grams else \
            range(len(self.patterns))
        own = len(s) - self.values(s, fed)
        need = min(own, max(4, int(own * 0.5)))
        return (self.patterns[k] for k in sorted(cand) if embeds(s, self.patterns[k][1], need))

    def producers(self, s, fed=""):
        """Source files with a literal that can print text containing s."""
        return {f for f, _ in self.matches(s, fed)}

    def prints(self, s, fed=""):
        """Can the core print s? Catches a reworded or removed message; what a field
        printed can't be checked."""
        return next(self.matches(s, fed), None) is not None


def embeds(s, pat, need):
    """True if all of s is part of some text pat prints (literal chars; WILD = a printf field,
    any run), with at least `need` of s's characters matched by literal ones. s may start and
    end anywhere inside the pattern."""
    # states: pattern position -> most literal characters matched on the way there
    states = closure({j: 0 for j in range(len(pat))}, pat)
    for c in s:
        nxt = {}
        for j, m in states.items():
            if j >= len(pat):
                continue
            if pat[j] is WILD:
                nxt[j] = max(nxt.get(j, -1), m)
            elif pat[j] == c:
                nxt[j + 1] = max(nxt.get(j + 1, -1), m + (not c.isspace()))
        if not nxt:
            return False
        states = closure(nxt, pat)
    return max(states.values()) >= need


def closure(states, pat):
    for j in sorted(states):
        if j < len(pat) and pat[j] is WILD and states.get(j + 1, -1) < states[j]:
            states[j + 1] = states[j]
    return states


def check(code=None):
    code = code or Code()
    errors = []
    tests, setups = test_files()
    data_files = sorted(os.listdir(DATA))
    data_text = "\n".join(read(os.path.join(DATA, f)) for f in data_files if f.endswith((".cfg", ".txt")))

    for path in tests + setups:
        text = read(path)
        where = rel(path)
        samples = {m.group(2): m.group(1) for m in re.finditer(
            r"- sample_pipe:\s*\{cmd:\s*([\w ]+?),\s*as:\s*(\w+)", text)}
        # Text the test feeds the game itself (an echo, a bind line): expected back as typed.
        fed = data_text + "\n" + "\n".join(unquote(m.group(2)) for m in FEEDS.finditer(text)) + \
            "\n" + "\n".join(re.findall(r"\bdst:\s*([^,}\n]+)", text))
        for ln, line in enumerate(text.splitlines(), 1):
            at = f"{where}:{ln}"
            body = line.split(" #", 1)[0]
            m = re.match(r"\s*- (\w+):", body)
            if m and m.group(1) not in code.steps:
                errors.append(f"{at}: step '{m.group(1)}' is not one the harness runs")
            for m in re.finditer(r"[{,]\s*cmd:\s*(\w+)", body):
                if m.group(1) not in code.channels:
                    errors.append(f"{at}: pipe channel '{m.group(1)}' is not registered")
            for m in re.finditer(r"[{,]\s*key:\s*(\w+)", body):
                if "cmd:" in body and f"{m.group(1)}=" not in code.raw:
                    errors.append(f"{at}: pipe field '{m.group(1)}' is written by no source line")
            m = re.search(r"\{cmd:\s*([\w ]+?),.*\bsince:\s*(\w+)", body)
            if m and samples.get(m.group(2)) != m.group(1):
                errors.append(f"{at}: since '{m.group(2)}' is not a sample_pipe of '{m.group(1)}' in this test")
            for m in re.finditer(r"\bfd_\w+", body):
                if m.group(0) not in code.names:
                    errors.append(f"{at}: '{m.group(0)}' is not a registered variable, command or old name")
            if re.match(r"\s*- (assert|sample)_mem:", body):
                m = re.search(r"\bname:\s*([\w.]+)", body)
                if m and m.group(1) not in code.entries:
                    errors.append(f"{at}: '{m.group(1)}' is not an address-table entry")
            # build/ is build output: absent in a clean checkout (CI), so not checked
            for m in re.finditer(r"\bsrc:\s*([^,}\s]+)", body):
                if m.group(1) != "null" and not m.group(1).startswith("build/") \
                        and not os.path.exists(os.path.join(REPO, m.group(1))):
                    errors.append(f"{at}: '{m.group(1)}' does not exist")
            for m in re.finditer(r"\{repo\}/([^\s\"']+)", body):
                if not m.group(1).startswith("build/") and not os.path.exists(os.path.join(REPO, m.group(1))):
                    errors.append(f"{at}: '{m.group(1)}' does not exist")
            for m in re.finditer(r"\b(?:run_)?setup:\s*(\w+)", body):
                if not os.path.exists(os.path.join(SETUPS, m.group(1) + ".yaml")):
                    errors.append(f"{at}: setup '{m.group(1)}' does not exist")
            if re.match(r"\s*- (assert_log|wait_log|assert_log_count|assert_pipe_contains):", body):
                for m in TEXT_KEY.finditer(body):
                    s = unquote(m.group(2))
                    for p in LOG_PREFIXES:
                        s = s[len(p):] if s.startswith(p) else s
                    if s.strip() and s not in fed and not code.prints(s.replace("{core_version}", FIELD), fed):
                        errors.append(f"{at}: no source string can print {s!r}")

    for f in data_files:
        if not any(f in read(p) for p in tests + setups) and f not in read(
                os.path.join(REPO, "tools", "make_override_fixtures.py")):
            errors.append(f"tests/game/data/{f}: no test uses it")

    named = set()
    for f, text in code.files.items():
        for m in re.finditer(r"^// Tests: (.+)$", text, re.M):
            for t in m.group(1).split():
                named.add(t)
                if not os.path.exists(os.path.join(REPO, t)):
                    errors.append(f"{f}: // Tests: names {t}, which does not exist")
    for path in tests:
        if os.path.dirname(path) == GAME and rel(path) not in named:
            errors.append(f"{rel(path)}: no // Tests: line names it (core or feature file)")

    used = "\n".join([data_text] + [read(p) for p in tests + setups] +
                     [read(os.path.join(UNIT, f)) for f in os.listdir(UNIT) if f.endswith((".cpp", ".py"))])
    for name in code.public:
        tested = re.search(r"(?<![\w-])" + re.escape(name) + r"(?![\w])", used) is not None
        if not tested and name not in UNTESTED:
            errors.append(f"'{name}' ({file_of(code, name)}): no test uses it; add one, or an "
                          f"UNTESTED entry with the reason (tools/test_check.py)")
        if tested and name in UNTESTED:
            errors.append(f"'{name}' is tested now: remove it from UNTESTED (tools/test_check.py)")
    for name in UNTESTED:
        if name not in code.public:
            errors.append(f"UNTESTED names '{name}', which is not registered (tools/test_check.py)")
    return errors


def file_of(code, name):
    for c in code.reg["cvars"] + code.reg["commands"]:
        if c["name"] == name:
            return c["file"]
    return "?"


# --- affected --------------------------------------------------------------------------

def git(*args):
    exe = "git"
    local = os.path.join(REPO, "environment", "local", "tools", "git", "cmd", "git.exe")
    if os.path.exists(local):
        exe = local
    return subprocess.run([exe, *args], cwd=REPO, capture_output=True, text=True).stdout


def affected(base="origin/main"):
    """The game tests a change touches: changed tests and their data, the // Tests: lines of
    changed source files (a feature's whole folder), and the tests that use a changed file's
    pipe channels, names, address entries or printed text. Harness changes: the suite."""
    changed = set(git("diff", "--name-only", f"{base}...HEAD").split())
    changed |= set(git("diff", "--name-only", "HEAD").split())
    changed |= set(git("ls-files", "--others", "--exclude-standard").split())
    code = Code()
    tests, setups = test_files()
    suite = [rel(t) for t in tests if os.path.dirname(t) == GAME]
    texts = {rel(t): read(t) for t in tests}
    picked, why = set(), {}

    def add(t, reason):
        if t in texts:
            picked.add(t)
            why.setdefault(t, reason)

    def uses(pred, reason):
        for t, text in texts.items():
            if pred(text):
                add(t, reason)

    for f in sorted(changed):
        if f.startswith("tools/harness/") and not f.startswith("tools/harness/setups/"):
            for t in suite:
                add(t, f"{f} (the harness)")
        elif f.startswith("tests/game/") and f.endswith(".yaml"):
            add(f, "changed")
        elif f.startswith(("tests/game/data/", "tools/harness/setups/")):
            b = os.path.splitext(os.path.basename(f))[0] if f.startswith("tools/") else os.path.basename(f)
            uses(lambda text: b in text, f)
        elif f == "core/addresses.json":
            ids = changed_entries(base)
            for src, text in code.files.items():
                if any(f"Id::{e.replace('.', '_')}" in text for e in ids):
                    for t in tests_of(code, src):
                        add(t, f"{f} ({src})")
            uses(lambda text: any(e in text for e in ids), f)
        elif f.startswith(("core/", "features/")) and f in code.files:
            for t in tests_of(code, f):
                add(t, f"{f} (// Tests:)")
            mine = [c for c, src in code.channels.items() if src == f]
            uses(lambda text: any(re.search(r"cmd:\s*" + c + r"\b", text) for c in mine), f"{f} (pipe)")
            names = [c["name"] for c in code.reg["cvars"] + code.reg["commands"] if c["file"] == f]
            names += [a["old"] for a in code.reg["aliases"] if a["file"] == f]
            uses(lambda text: any(re.search(r"\b" + re.escape(n) + r"\b", text) for n in names), f"{f} (names)")
            for t, text in texts.items():
                if t in picked:
                    continue
                for m in re.finditer(r"\b(?:contains|absent):\s*(" + QUOTED + ")", text):
                    s = unquote(m.group(1))
                    for p in LOG_PREFIXES:
                        s = s[len(p):] if s.startswith(p) else s
                    if len(s) >= 5 and f in code.producers(s):
                        add(t, f"{f} (text)")
                        break
    out = sorted(picked)
    for t in out:
        print(f"  {t:45} {why[t]}")
    if out:
        print("python -I tools/harness/run_test.py " + " ".join(out))
    else:
        print("no game test touched")
    return out


def tests_of(code, f):
    """The // Tests: lines of a source file; for a feature file, every file of its folder."""
    files = [f]
    if f.startswith("features/"):
        folder = f.rsplit("/", 1)[0] + "/"
        files = [g for g in code.files if g.startswith(folder)]
    out = []
    for g in files:
        for m in re.finditer(r"^// Tests: (.+)$", code.files[g], re.M):
            out += [t for t in m.group(1).split() if t.startswith("tests/game/")]
    return out


def changed_entries(base):
    def entries(text):
        try:
            t = json.loads(text)
        except ValueError:
            return {}
        return {n: json.dumps(e, sort_keys=True) for v in t["versions"].values() for n, e in v["entries"].items()}
    old = entries(git("show", f"{base}:core/addresses.json"))
    new = entries(read(os.path.join(REPO, "core", "addresses.json")))
    return {n for n in set(old) | set(new) if old.get(n) != new.get(n)}


def main(argv):
    if argv[:1] == ["affected"]:
        affected(*argv[1:2])
        return 0
    errors = check()
    for e in errors:
        print(e)
    print(f"test_check: {len(errors)} problem(s)" if errors else "test_check: tests and code in step")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
