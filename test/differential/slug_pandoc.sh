#!/usr/bin/env bash
# Compare dmb's -dcs-id: auto with pandoc's auto_identifiers, one heading per line of
# slug_cases.txt. pandoc runs with smart punctuation off, so both see the same plain text.
# Not part of `make test`: it needs pandoc. Usage, from the repo root:
#   test/differential/slug_pandoc.sh [path/to/duckdb] [path/to/dmb.duckdb_extension]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
cases="$here/slug_cases.txt"
duckdb=${1:-build/release/duckdb}
ext=${2:-build/release/extension/dmb/dmb.duckdb_extension}
command -v pandoc >/dev/null || { echo "FAIL: pandoc not found" >&2; exit 2; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# pandoc: every heading in one document, so repeats get -1, -2 there too
while IFS= read -r line; do printf '# %s\n\n' "$line"; done < "$cases" \
  | pandoc -f markdown-smart -t html | sed -n 's/.*<h1 id="\([^"]*\)".*/\1/p' > "$work/pandoc.txt"

# dmb: the same headings as one document, through -dcs-id: auto
{
  echo "LOAD '$ext';"
  echo ".mode list"
  echo ".headers off"
  printf "SELECT unnest(list_transform(dcs_apply(list({'kind': 'block', 'element_type': 'heading', 'content': h, 'level': 1, 'encoding': 'text', 'attributes': MAP {'heading_level': '1'}, 'element_order': i::INTEGER} ORDER BY i), 'heading { -dcs-id: auto; }'), lambda x: x.attributes['id'])) FROM (SELECT row_number() OVER () - 1 AS i, column0 AS h FROM read_csv('%s', header := false, delim := '\\\\x01', quote := '', escape := '', columns := {'column0': 'VARCHAR'}, strict_mode := false));\n" "$cases"
} > "$work/dmb.sql"
"$duckdb" -no-agent -unsigned < "$work/dmb.sql" > "$work/dmb.txt"

n_cases=$(wc -l < "$cases")
n_pandoc=$(wc -l < "$work/pandoc.txt")
n_dmb=$(wc -l < "$work/dmb.txt")
if [ "$n_cases" -eq 0 ] || [ "$n_pandoc" -ne "$n_cases" ] || [ "$n_dmb" -ne "$n_cases" ]; then
  echo "FAIL: $n_cases cases, pandoc gave $n_pandoc ids, dmb gave $n_dmb" >&2
  exit 1
fi
if paste -d'|' "$cases" "$work/pandoc.txt" "$work/dmb.txt" | awk -F'|' '$2 != $3 { print "DIFF: [" $1 "] pandoc=" $2 " dmb=" $3; bad = 1 } END { exit bad }'; then
  echo "OK: $n_cases headings, dmb matches pandoc $(pandoc --version | head -1 | cut -d' ' -f2)"
else
  exit 1
fi
