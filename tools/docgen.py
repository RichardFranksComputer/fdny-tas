"""The player reference for FDNYCore: every default key, feature, variable and command, read from the
source on every build, so the player-facing list can't go stale.

    python -I tools/docgen.py sheet <out.md> [--asi <FDNYCore.asi>]   # the reference
    python -I tools/docgen.py sheet docs/reference.md                 # the source's (pre-commit hook)
    python -I tools/docgen.py json                                    # what was read
    python -I tools/docgen.py notes <version> [CHANGES.md]            # that version's release notes

What it reads (string literals at the registration calls; adjacent literals joined):
  cmd::Cvar g_x = {"name", "default", "help", readOnly, archive, ...}   + cmd::RegisterCvar(&g_x)
  cmd::RegisterCommand("name", "help", fn)        cmd::SetPracticeOnly("name")
  cmd::RegisterAlias("old", "current")            binds::BindDefault("key", "line")
  binds::DescribeKey("key", "action", "when")     FDCORE_MODULE(id, "name", ..., order)
  "// User: <Title> -- <one sentence>"            the file's entry in the reference
Inside #ifdef FDCORE_DEBUG: Debug-only (left out of the reference, kept in `json`). A help text
starting "internal:" is left out too.

Exit 1 when something can't be read: a registration whose arguments aren't literals, a
RegisterCvar(&x) with no `cmd::Cvar x = {...}`, a file registering things with no
"// User:" line. tests/game/docs_registry.yaml checks this reading against what the Debug build
actually registers (fd_dbg_registry).
"""
import hashlib
import json
import os
import re
import sys

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
CORE = os.path.join(REPO, "core")
FEAT = os.path.join(REPO, "features")
# Core files' sections come first, in this order (others after them, by name).
CORE_ORDER = ["cmd.cpp", "binds.cpp", "profile.cpp", "gfx.cpp", "textdraw.cpp", "patches.cpp"]

USER = re.compile(r"^// User: (.+?) -- (.+)$", re.M)
# The registration calls and their argument counts: any call of these that can't be read is
# an error, never skipped.
ARITY = {"RegisterCvar": 1, "RegisterCommand": 3, "SetPracticeOnly": 1, "RegisterAlias": 2,
         "BindDefault": 2, "DescribeKey": 3}


class ReadError(Exception):
    pass


def tokens(text):
    """(kind, value, line, debug) for code tokens: 'id', 'str' (adjacent literals joined,
    escapes decoded), 'num', 'p' (one punctuation char). Comments dropped; preprocessor
    lines only steer the Debug flag (#ifdef FDCORE_DEBUG / #else / #endif)."""
    out, stack = [], []
    i, n, line = 0, len(text), 1
    at_line_start = True

    def debug():
        return any(stack)

    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
            at_line_start = True
            continue
        if c in " \t\r":
            i += 1
            continue
        if at_line_start and c == "#":
            j = i
            while True:  # continuation lines
                e = text.find("\n", j)
                e = n if e < 0 else e
                if text[j:e].rstrip("\r").endswith("\\"):
                    j = e + 1
                    line += 1
                    continue
                break
            d = text[i + 1:e].strip()
            word = d.split()[0] if d.split() else ""
            arg = d.split()[1] if len(d.split()) > 1 else ""
            if word == "ifdef":
                stack.append(arg == "FDCORE_DEBUG")
            elif word in ("ifndef", "if"):
                stack.append(False)
            elif word == "else" and stack:
                stack[-1] = False
            elif word == "endif" and stack:
                stack.pop()
            i = e
            continue
        at_line_start = False
        if text.startswith("//", i):
            e = text.find("\n", i)
            i = n if e < 0 else e
            continue
        if text.startswith("/*", i):
            e = text.find("*/", i + 2)
            e = n if e < 0 else e + 2
            line += text.count("\n", i, e)
            i = e
            continue
        if c == '"':
            j, s = i + 1, []
            while j < n and text[j] != '"':
                if text[j] == "\\" and j + 1 < n:
                    s.append({"n": "\n", "t": "\t", "r": "\r", "0": "\0"}.get(text[j + 1], text[j + 1]))
                    j += 2
                else:
                    s.append(text[j])
                    j += 1
            val = "".join(s)
            if out and out[-1][0] == "str":
                k, v, ln, dbg = out[-1]
                out[-1] = (k, v + val, ln, dbg)
            else:
                out.append(("str", val, line, debug()))
            i = j + 1
            continue
        if c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            out.append(("num", text[i:j + 1], line, debug()))
            i = j + 1
            continue
        if c.isalpha() or c == "_":
            j = i
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            out.append(("id", text[i:j], line, debug()))
            i = j
            continue
        if c.isdigit():
            j = i
            while j < n and (text[j].isalnum() or text[j] in "._"):
                j += 1
            out.append(("num", text[i:j], line, debug()))
            i = j
            continue
        out.append(("p", c, line, debug()))
        i += 1
    return out


def args_at(toks, i, open_="(", close=")"):
    """Arguments of the bracket that starts at toks[i]: a list of token lists."""
    if i >= len(toks) or toks[i][1] != open_:
        return None
    depth, cur, res = 0, [], []
    for t in toks[i:]:
        if t[0] == "p" and t[1] in "([{":
            depth += 1
            if depth == 1:
                continue
        elif t[0] == "p" and t[1] in ")]}":
            depth -= 1
            if depth == 0:
                res.append(cur)
                return res
        if depth == 1 and t[0] == "p" and t[1] == ",":
            res.append(cur)
            cur = []
        else:
            cur.append(t)
    return None


def lit(arg, where, what):
    if len(arg) == 1 and arg[0][0] == "str":
        return arg[0][1]
    raise ReadError(f"{where}: {what} is not a string literal ({' '.join(t[1] for t in arg) or 'nothing'})")


def read_source():
    files = [os.path.join(CORE, f) for f in sorted(os.listdir(CORE)) if f.endswith(".cpp")] + \
            [os.path.join(FEAT, d, f) for d in sorted(os.listdir(FEAT)) if os.path.isdir(os.path.join(FEAT, d))
             for f in sorted(os.listdir(os.path.join(FEAT, d))) if f.endswith(".cpp")]
    reg = {"sections": [], "cvars": [], "commands": [], "aliases": [], "binds": [], "keys": [], "practice": []}
    cvar_defs, cvar_regs, errors = {}, [], []
    for path in files:
        rel = os.path.relpath(path, REPO).replace("\\", "/")
        with open(path, encoding="utf-8-sig") as f:
            text = f.read()
        toks = tokens(text)
        um = USER.search(text)
        section = {"file": rel, "title": um.group(1) if um else None, "summary": um.group(2) if um else None,
                   "module": None, "order": None, "debug": False, "feature": path.startswith(FEAT)}
        registers = False
        for i, t in enumerate(toks):
            if t[0] != "id":
                continue
            name, ln, dbg = t[1], t[2], t[3]
            where = f"{rel}:{ln}"
            try:
                if name == "Cvar" and i + 2 < len(toks) and toks[i + 1][0] == "id" and toks[i + 2][1] == "=":
                    a = args_at(toks, i + 3, "{", "}")
                    if a and len(a) >= 5 and a[0] and a[0][0][0] == "str":
                        cvar_defs[toks[i + 1][1]] = {
                            "name": lit(a[0], where, "name"), "default": lit(a[1], where, "default"),
                            "help": lit(a[2], where, "help"), "read_only": a[3][0][1] == "true",
                            "archive": a[4][0][1] == "true"}
                    continue
                if i > 0 and toks[i - 1][1] in (".", ">"):
                    continue  # a member called the same, not a registration
                if name == "FDCORE_MODULE":
                    a = args_at(toks, i + 1)
                    if a and len(a) == 7 and a[1] and a[1][0][0] == "str":  # not the macro's own #define
                        section.update(module=lit(a[1], where, "module name"), order=int(a[6][0][1]), debug=dbg)
                    continue
                if name not in ARITY:
                    continue
                if i > 0 and toks[i - 1][0] == "id" and toks[i - 1][1] in ("void", "bool", "int"):
                    continue  # the function's own definition
                a = args_at(toks, i + 1)
                if a is None or len(a) != ARITY[name]:
                    raise ReadError(f"{where}: {name}(...) with {len(a) if a else 0} argument(s), want {ARITY[name]}")
                if name == "RegisterCvar":
                    if len(a[0]) != 2 or a[0][0][1] != "&" or a[0][1][0] != "id":
                        raise ReadError(f"{where}: RegisterCvar wants &<a cmd::Cvar defined in this file>")
                    cvar_regs.append((a[0][1][1], rel, where, dbg))
                elif name == "RegisterCommand":
                    reg["commands"].append({"name": lit(a[0], where, "name"), "help": lit(a[1], where, "help"),
                                            "file": rel, "debug": dbg})
                elif name == "SetPracticeOnly":
                    reg["practice"].append(lit(a[0], where, "practice-only name"))
                elif name == "RegisterAlias":
                    reg["aliases"].append({"old": lit(a[0], where, "old name"), "current": lit(a[1], where, "name"),
                                           "file": rel, "debug": dbg})
                elif name == "BindDefault":
                    reg["binds"].append({"key": lit(a[0], where, "key"), "line": lit(a[1], where, "line"),
                                         "file": rel, "debug": dbg})
                elif name == "DescribeKey":
                    reg["keys"].append({"key": lit(a[0], where, "key"), "action": lit(a[1], where, "action"),
                                        "when": lit(a[2], where, "when"), "file": rel, "debug": dbg})
                registers = registers or name != "SetPracticeOnly"
            except ReadError as e:
                errors.append(str(e))
        if registers and not um:
            errors.append(f'{rel}: registers player-facing items but has no "// User: <Title> -- <sentence>" line')
        if registers or (section["module"] and um):
            reg["sections"].append(section)
    for var, rel, where, dbg in cvar_regs:
        d = cvar_defs.get(var)
        if not d:
            errors.append(f"{where}: RegisterCvar(&{var}) but no `cmd::Cvar {var} = {{\"name\", ...}}` found")
            continue
        reg["cvars"].append(dict(d, file=rel, debug=dbg))
    names = {c["name"] for c in reg["cvars"]} | {c["name"] for c in reg["commands"]}
    for p in reg["practice"]:
        if p not in names:
            errors.append(f"SetPracticeOnly(\"{p}\"): no variable or command of that name")
    for c in reg["cvars"] + reg["commands"]:
        c["practice"] = c["name"] in reg["practice"]
    if errors:
        raise ReadError("\n".join(errors))
    return reg


def version():
    with open(os.path.join(REPO, "VERSION"), encoding="utf-8") as f:
        return f.read().strip()


def md(s):
    return s.replace("|", "\\|")


def section_order(reg):
    def key(s):
        base = os.path.basename(s["file"])
        if not s["feature"]:
            return (0, CORE_ORDER.index(base) if base in CORE_ORDER else len(CORE_ORDER), base)
        return (1, s["order"] if s["order"] is not None else 999, base)
    seen, out = set(), []
    for s in sorted(reg["sections"], key=key):
        if s["debug"] or s["title"] in seen:
            continue
        seen.add(s["title"])
        out.append(s)
    return out


def shown(item):
    return not item["debug"] and not item.get("help", "").startswith("internal:")


def sheet(reg, asi=None):
    title_of = {s["file"]: s["title"] for s in reg["sections"]}
    rank = {t: i for i, t in enumerate(s["title"] for s in section_order(reg))}
    by_feature = lambda items: sorted(items, key=lambda c: rank.get(title_of[c["file"]], 999))  # stable
    cvars = by_feature(c for c in reg["cvars"] if shown(c))
    cmds = by_feature(c for c in reg["commands"] if shown(c))
    reg = dict(reg, binds=by_feature(reg["binds"]), keys=by_feature(reg["keys"]))
    by_name = {c["name"]: c for c in cvars + cmds}
    v = version()
    L = [f"# FDNYCore {v} reference: keys, features and settings", "",
         "Generated from the source by `tools/docgen.py` on every build; do not edit. To change a",
         "line here, change the help text or `// User:` line in the code. What changed between",
         "releases: `CHANGES.md` in the repository.", ""]
    if not asi:
        L[-1:] = ["", "**This is the reference for the current source** (regenerated by the pre-commit hook and",
                  "every build). Downloads are the GitHub releases (Releases, on the right of the repository",
                  "page); each carries the reference of its own build in its zip, with its SHA-256.", ""]
    if asi:
        data = open(asi, "rb").read()
        L += ["| | |", "|---|---|",
              f"| File | `FDNYCore-{v}.asi` (Release build) |",
              f"| Version | {v} (`fd_version` in the console) |",
              f"| SHA-256 | `{hashlib.sha256(data).hexdigest()}` |",
              f"| Size | {len(data):,} bytes |", "",
              f"**Install:** copy `FDNYCore-{v}.asi` next to `FDNYFirefighter.exe` and remove any older",
              "`FDNYCore*.asi` (only the first copy runs; the log names the others). It loads through",
              "Ultimate ASI Loader (`dinput8.dll`), like the other `.asi` plugins. Open the game's console",
              "with the ` key; `cvarlist`, `cmdlist` and `bindlist` show everything below in the game.", ""]

    L += ["## Keys", "",
          "Keys act only while the game has focus and the console is closed.", "",
          "**Default binds**: rebind with `bind <key> \"<command>\"`, remove with `unbind <key>`; only",
          "your changes are saved (FDNYCore.cfg). **Practice**: refused while `fd_profile` is `run`.", "",
          "| Key | Does | Runs | Practice |", "|---|---|---|---|"]
    for b in (b for b in reg["binds"] if not b["debug"]):
        words = b["line"].split()
        target = words[1] if words[0] == "toggle" and len(words) > 1 else words[0]
        t = by_name.get(target)
        does = t["help"] if t else b["line"]
        if words[0] == "toggle":
            does = "on / off: " + re.sub(r"^1 = ", "", does)
        L.append(f"| **{b['key'].upper()}** | {md(does)} | `{md(b['line'])}` | "
                 f"{'yes' if t and t['practice'] else ''} |")
    L += ["", "**Feature keys**: read by the feature itself while it is on; they can't be rebound.", "",
          "| Key | Does | When |", "|---|---|---|"]
    for k in (k for k in reg["keys"] if not k["debug"]):
        keys = " ".join(f"**{w.upper()}**" for w in k["key"].split())
        L.append(f"| {keys} | {md(k['action'])} | {md(k['when'])} |")
    L += [""]

    L += ["## Features", ""]
    for s in section_order(reg):
        files = {x["file"] for x in reg["sections"] if x["title"] == s["title"]}
        vs = [f"`{c['name']}`" for c in cvars if c["file"] in files]
        cs = [f"`{c['name']}`" for c in cmds if c["file"] in files]
        ks = [b["key"].upper() for b in reg["binds"] if b["file"] in files and not b["debug"]] + \
             [k["key"].upper() for k in reg["keys"] if k["file"] in files and not k["debug"]]
        parts = []
        if vs:
            parts.append("variables " + ", ".join(vs))
        if cs:
            parts.append("commands " + ", ".join(cs))
        if ks:
            parts.append("keys " + ", ".join(ks))
        L.append(f"- **{md(s['title'])}**: {md(s['summary'])}" + (f" ({'; '.join(parts)})" if parts else ""))
    L += [""]

    L += ["## Variables", "",
          "Type the name to see its value, `name value` to set it. **Saved**: kept in FDNYCore.cfg",
          "when changed. **Practice**: refused while `fd_profile` is `run`.", "",
          "| Variable | Default | Values and meaning | Saved | Practice | Feature |",
          "|---|---|---|---|---|---|"]
    for c in cvars:
        dflt = f"`{c['default']}`" if c["default"] else "(set by the build)"
        meaning = ("read-only: " if c["read_only"] else "") + c["help"]
        L.append(f"| `{c['name']}` | {md(dflt)} | {md(meaning)} | {'yes' if c['archive'] else ''} | "
                 f"{'yes' if c['practice'] else ''} | {md(title_of[c['file']])} |")
    L += [""]

    L += ["## Commands", "", "| Command | What it does | Practice | Feature |", "|---|---|---|---|"]
    for c in cmds:
        L.append(f"| `{c['name']}` | {md(c['help'])} | {'yes' if c['practice'] else ''} | "
                 f"{md(title_of[c['file']])} |")
    L += [""]

    aliases = [a for a in reg["aliases"] if not a["debug"]]
    if aliases:
        L += ["## Old names", "", "These still work and mean the new name.", "",
              "| Old name | Now |", "|---|---|"]
        L += [f"| `{a['old']}` | `{a['current']}` |" for a in aliases]
        L += [""]
    return "\n".join(L)


def release_notes(text, version):
    """The "## <version>" section of CHANGES.md (up to the next "## "), trimmed; None if
    there isn't one. The release workflow puts it in the GitHub release's notes (D45)."""
    m = re.search(rf"(?ms)^## {re.escape(version)}[ \t]*\r?\n(.*?)(?=^## |\Z)", text)
    return m.group(1).strip() if m else None


def main(argv):
    if argv and argv[0] == "notes" and len(argv) >= 2:
        path = argv[2] if len(argv) > 2 else os.path.join(REPO, "CHANGES.md")
        notes = release_notes(open(path, encoding="utf-8").read(), argv[1])
        if notes is None:
            print(f"docgen: {path} has no '## {argv[1]}' section (add the release notes)", file=sys.stderr)
            return 1
        sys.stdout.write(notes + "\n")
        return 0
    if not argv or argv[0] not in ("sheet", "json"):
        print(__doc__)
        return 2
    try:
        reg = read_source()
    except ReadError as e:
        print("docgen: cannot read the source:\n  " + str(e).replace("\n", "\n  "))
        return 1
    if argv[0] == "json":
        print(json.dumps(reg, indent=1))
        return 0
    out = argv[1]
    asi = argv[argv.index("--asi") + 1] if "--asi" in argv else None
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(sheet(reg, asi))
    n = lambda k: sum(1 for x in reg[k] if not x["debug"])
    print(f"docgen: {out}: {n('cvars')} variables, {n('commands')} commands, {n('binds')} default keys, "
          f"{n('keys')} feature keys, {len(section_order(reg))} features")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
