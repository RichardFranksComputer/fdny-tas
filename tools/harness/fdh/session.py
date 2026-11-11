"""Game sessions shared between tests (owner 2026-10-25: launch and exit only when a session
can't be reused).

A test can join a session when nothing it needs is fixed at launch beyond what the session
already has: the same core build, game config and files read at load (.asi plugins, the
address-override table, autoexec.cfg), no environment switches, no restart, and the
Training setup. Its FDNYCore.cfg is applied at runtime instead: before each test the core is
reset to the defaults read from the source (docgen), the test's cfg is exec'd and Training
is reloaded. A test that needs its own launch says `own_session: true`.
"""
import os
import shutil
import sys

LOAD_TIME_FILES = {"fdnyfirefighter.cfg", "autoexec.cfg", "fdnycore.addroverride.txt"}
RESET_CFG = "fdtest_reset.cfg"  # written into the game folder, run with `exec`
TEST_CFG = "fdtest_core.cfg"


def is_load_time(dst):
    d = dst.lower()
    return d in LOAD_TIME_FILES or d.endswith(".asi")


def step_kind(s):
    return next(iter(s)) if isinstance(s, dict) else s


def key(test, core, game_dir):
    """What a test's launch depends on, or None if it needs a launch of its own. A file the
    test wants absent that isn't in the game folder anyway changes nothing."""
    if (core is None or test.get("env") or test.get("own_session") or test.get("setup") != "load_training"
            or any(step_kind(s) == "restart" for s in test.get("steps", []))):
        return None
    load = tuple(sorted((f["dst"].lower(), f.get("src") or "") for f in test.get("pre_files", [])
                        if is_load_time(f["dst"])
                        and (f.get("src") or os.path.exists(os.path.join(game_dir, f["dst"])))))
    return (test.get("core", core), load)


def reset_lines(repo_dir):
    """The core back to its defaults: practice profile first (it gates the rest), every default
    bind (a default key on its default line is not written to FDNYCore.cfg), every variable."""
    sys.path.insert(0, os.path.join(repo_dir, "tools"))
    import docgen
    reg = docgen.read_source()
    lines = ['fd_profile "practice"', "unbindall"]
    lines += [f'bind {b["key"]} "{b["line"]}"' for b in reg["binds"]]
    lines += [f'bind {m["name"]} default' for m in reg["modes"]]  # mode keys a test rebound
    lines += [f'{c["name"]} "{c["default"]}"' for c in reg["cvars"]
              if not c["read_only"] and c["name"] != "fd_profile"]
    return lines


class Session:
    def __init__(self, key_):
        self.key = key_
        self.g = None              # game.Game while the game runs
        self.resolver = self.version = None
        self.startup_end = 0       # FDNYCore.log length after launch + setup (shared by every test)
        self.placed = []           # load-time files swapped in, restored by close()

    @property
    def alive(self):
        return self.g is not None and self.g.proc is not None and self.g.check_crash() is None

    def place(self, game_dir, pre_files, repo_dir):
        """The load-time files, and FDNYCore.cfg absent: tests apply theirs at runtime."""
        self.placed.append((os.path.join(game_dir, RESET_CFG), ""))  # removed again by close()
        for f in [x for x in pre_files if is_load_time(x["dst"])] + [{"dst": "FDNYCore.cfg", "src": None}]:
            dst = os.path.join(game_dir, f["dst"])
            backup = dst + ".fdh-backup"
            if os.path.exists(dst):
                shutil.copy2(dst, backup)
            if f.get("src"):
                shutil.copy2(os.path.join(repo_dir, f["src"]), dst)
            elif os.path.exists(dst):
                os.remove(dst)
            self.placed.append((dst, backup))

    def close(self):
        if self.g:
            self.g.kill()
        self.g = None
        for dst, backup in self.placed:
            try:
                if os.path.exists(backup):
                    os.replace(backup, dst)
                elif os.path.exists(dst):
                    os.remove(dst)
            except OSError:
                pass
        self.placed = []
