#!/usr/bin/env bash
# Run the wasm dependency-symbol check (check_wasm_imports.mjs) over every
# dmb.duckdb_extension.wasm under the given directories: local build dirs or
# downloaded CI artifacts. No duckdb-wasm runtime needed.
#
# Usage: [EXPECT_MODULES=n] test/wasm/run_wasm_checks.sh [dir ...]
#   defaults to build/wasm_mvp build/wasm_eh build/wasm_threads if present.
#   EXPECT_MODULES: fail unless exactly n modules were checked (CI sets it, so
#   a missing build cannot pass as a smaller run).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$here/../.." && pwd)"

# Must be linked into the module, never left as an import:
#   RE2 (DuckDB's duckdb_re2): members, const members, vtables, typeinfo.
#     duckdb-wasm exports only DuckDB's Regex wrapper, not the RE2 class.
#   tree-sitter runtime and the CSS grammar (ts_*, tree_sitter_*).
# utf8proc is not listed: dmb calls duckdb::utf8proc_*, which the engine exports.
FORBID=( --forbid '^_Z(NK?|T[VTIS]N)10duckdb_re2' --forbid '^(ts_|tree_sitter_)' )

dirs=("$@")
if [ ${#dirs[@]} -eq 0 ]; then
  for d in build/wasm_mvp build/wasm_eh build/wasm_threads; do
    [ -d "$repo_root/$d" ] && dirs+=("$repo_root/$d")
  done
fi
if [ ${#dirs[@]} -eq 0 ]; then
  echo "no wasm build dirs found; build first (e.g. make wasm_eh)" >&2
  exit 2
fi

found=0
rc=0
for d in "${dirs[@]}"; do
  while IFS= read -r -d '' wasm; do
    found=$((found + 1))
    echo "==> checking $wasm"
    node "$here/check_wasm_imports.mjs" "$wasm" "${FORBID[@]}" || rc=1
  done < <(find "$d" -name 'dmb.duckdb_extension.wasm' -print0 2>/dev/null)
done

if [ "$found" -eq 0 ]; then
  echo "no dmb.duckdb_extension.wasm found under: ${dirs[*]}" >&2
  exit 2
fi
echo "checked $found module(s)"
if [ -n "${EXPECT_MODULES:-}" ] && [ "$found" -ne "$EXPECT_MODULES" ]; then
  echo "FAIL: expected $EXPECT_MODULES module(s), checked $found" >&2
  rc=1
fi
exit $rc
