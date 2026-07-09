#!/usr/bin/env python3
"""Generate compile_commands.json for the OpenTyrian tree.

Self-contained: it does *not* need bear/compiledb. The Makefile `compdb`
target invokes this with the fully-resolved compiler flags exported in the
environment, so the emitted database mirrors exactly what `make` would run.

Environment variables (all set by the Makefile target):
  COMPDB_CC     - the compiler (e.g. "gcc" / "cc")
  COMPDB_DIR    - the build directory (absolute; goes in each "directory")
  COMPDB_FLAGS  - all CPPFLAGS + CFLAGS as a single shell-quoted string
  COMPDB_SRCS   - space-separated list of source files (e.g. "src/a.c src/b.c")

Using shlex.split on COMPDB_FLAGS means embedded shell quoting (such as the
-DOPENTYRIAN_VERSION='"..."' define) is unquoted correctly before it reaches
clangd, and json.dumps re-escapes it for the database.
"""

import json
import os
import shlex
import sys


def main():
    cc = os.environ.get("COMPDB_CC", "cc")
    directory = os.environ.get("COMPDB_DIR", os.getcwd())
    flags = shlex.split(os.environ.get("COMPDB_FLAGS", ""))
    srcs = os.environ.get("COMPDB_SRCS", "").split()

    if not srcs:
        sys.stderr.write("gen_compile_commands.py: no sources (COMPDB_SRCS empty)\n")
        return 1

    entries = []
    for src in srcs:
        # obj/foo.o for src/foo.c, mirroring the Makefile's object layout.
        base = src[len("src/"):] if src.startswith("src/") else src
        obj = "obj/" + base[:-len(".c")] + ".o" if base.endswith(".c") else "obj/" + base
        arguments = shlex.split(cc) + flags + ["-c", "-o", obj, src]
        entries.append({
            "directory": directory,
            "file": src,
            "arguments": arguments,
        })

    with open("compile_commands.json", "w") as f:
        json.dump(entries, f, indent=2)
        f.write("\n")

    sys.stderr.write("gen_compile_commands.py: wrote compile_commands.json (%d entries)\n" % len(entries))
    return 0


if __name__ == "__main__":
    sys.exit(main())
