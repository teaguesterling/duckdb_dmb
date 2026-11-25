#!/usr/bin/env python3
"""Collect the sheet literals of test/sql/*.test into test/data/roundtrip_sheets.csv.

The round-trip test (test/sql/dcs_roundtrip.test) applies every sheet here as text, as its DCS value,
and as dcs_format of that value, and asks for the same document each time. Re-run this script when
the tests gain sheets:

    test/data/collect_sheets.py path/to/duckdb path/to/dmb.duckdb_extension

A sheet is any SQL string literal that holds a rule ("{" after a selector, or an at-rule). `clean` is
true when dcs_check gives no rows for it, so its strict form must also match the text.
"""
import csv
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
LITERAL = re.compile(r"'((?:[^']|'')*)'")
RULE = re.compile(r"^\s*(@[a-z-]+|[^{}\"'\[\]]*[a-zA-Z0-9*#.:\])])\s*[^{}]*\{")


def literals():
    seen = {}
    for path in sorted((ROOT / "test" / "sql").glob("*.test")):
        if path.name in ("dcs_roundtrip.test",):
            continue
        # comment lines hold apostrophes ("can't") that would shift the literal matching
        body = "\n".join(l for l in path.read_text().splitlines() if not l.lstrip().startswith("#"))
        for m in LITERAL.finditer(body):
            text = m.group(1).replace("''", "'")
            if "{" not in text or "}" not in text or not RULE.match(text):
                continue
            if text.startswith("{") or "MAP {" in text or "=>" in text:
                continue  # a struct or map literal, not a sheet
            seen.setdefault(text, path.name)
    return seen


def clean_flags(duckdb, ext, sheets):
    flags = []
    for text in sheets:
        sql = "LOAD '%s'; SELECT count(*) FROM dcs_check('%s');" % (ext, text.replace("'", "''"))
        out = subprocess.run([duckdb, "-no-agent", "-unsigned", "-noheader", "-list", "-c", sql],
                             capture_output=True, text=True)
        if out.returncode != 0:
            raise SystemExit("dcs_check failed for %r: %s" % (text, out.stderr))
        flags.append(out.stdout.strip().splitlines()[-1] == "0")
    return flags


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    sheets = literals()
    flags = clean_flags(sys.argv[1], sys.argv[2], list(sheets))
    out = ROOT / "test" / "data" / "roundtrip_sheets.csv"
    with out.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["source", "sheet", "clean"])
        for (text, source), flag in zip(sheets.items(), flags):
            w.writerow([source, text, "true" if flag else "false"])
    print("%d sheets, %d clean -> %s" % (len(sheets), sum(flags), out.relative_to(ROOT)))


if __name__ == "__main__":
    main()
