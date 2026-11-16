#!/usr/bin/env bash
# Differential: dmb's matcher (dcs_select) against sitting_duck's ast_select_from, on the same
# document, EXCEPT both ways (spec: "the alignment check"; sitting_duck spec 046's own check).
# Blocks are projected to read_ast's schema as the duckeye CLI does (below). Only block
# rows are projected: dmb never matches inline rows. Left out, with the reason:
#   - selector lists ('A, B'): ast_select returns 0 rows for them (sitting_duck#330);
#   - [attr] selectors: sitting_duck's attributes are a fixed allowlist, so the CLI strips them;
#   - dmb's own pseudo-classes (:contains, :section, :h, role pseudo-classes): ast_select lacks them.
# Not part of `make test`: it needs sitting_duck. Usage, from the repo root:
#   SITTING_DUCK_EXT=... test/differential/ast_select.sh [duckdb] [dmb ext]
set -uo pipefail
duckdb=${1:-build/release/duckdb}
ext=${2:-build/release/extension/dmb/dmb.duckdb_extension}
: "${SITTING_DUCK_EXT:?set SITTING_DUCK_EXT}"
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
printf 'echo hi\n' > "$work/tiny.sh"   # read_ast() needs one parseable file for its typed NULLs

cat > "$work/setup.sql" <<SQL
LOAD '$ext';
LOAD '$SITTING_DUCK_EXT';
SQL
cat > "$work/doc.sql" <<'SQL'
CREATE TEMP TABLE doc AS SELECT * FROM (VALUES
  (0, 'heading', 1, 'intro'), (1, 'paragraph', 1, ''), (2, 'heading', 1, 'comments'),
  (3, 'paragraph', 1, ''), (4, 'list', 1, ''), (5, 'list_item', 2, ''), (6, 'paragraph', 3, ''),
  (7, 'list_item', 2, ''), (8, 'list', 3, ''), (9, 'list_item', 4, ''), (10, 'list_item', 4, ''),
  (11, 'list_item', 2, ''), (12, 'blockquote', 1, ''), (13, 'paragraph', 2, ''), (14, 'paragraph', 2, ''),
  (15, 'heading', 1, 'end'), (16, 'paragraph', 1, '')
) t(ord, ty, lvl, nm);
CREATE TEMP TABLE t AS SELECT ord, lvl, ty, nm, 'doc' AS part FROM doc;
CREATE TEMP TABLE blocks AS SELECT list({'kind': 'block', 'element_type': ty, 'content': 'x', 'level': lvl,
    'encoding': 'text', 'attributes': CASE WHEN nm = '' THEN MAP {}::MAP(VARCHAR, VARCHAR) ELSE MAP {'id': nm} END,
    'element_order': ord::INTEGER} ORDER BY ord) AS b FROM doc;
SQL
# The projection is the duckeye CLI's own (duckeye, the -Q path near line 2290): a synthetic
# document root, parents and spans from one windowed pass per level, and a table created from a
# zero-row read_ast() and filled BY NAME, so a column sitting_duck adds later is NULL rather than
# a Binder Error. (duckeye's prototypes/leveled_ast.sql hand-lists the columns and no longer binds:
# it lacks `receiver`, measured 2026-11-15.) One change: name is the block's id, because dmb's
# `#x` means attributes['id']. Block element_order k is node k + 2.
cat > "$work/proj.sql" <<SQL
CREATE TEMP TABLE qn AS
  SELECT 1 AS node_id, 0 AS ord, (SELECT min(lvl) FROM t) - 1 AS lvl, 'document' AS ty, '' AS nm
  UNION ALL SELECT ord + 2, ord + 1, lvl, ty, nm FROM t;
CREATE TEMP TABLE qe AS
  WITH lv AS (SELECT DISTINCT lvl AS l FROM qn),
       x AS (SELECT n.node_id, n.ord, n.lvl, lv.l,
               max(CASE WHEN n.lvl < lv.l THEN n.node_id END) OVER (PARTITION BY lv.l ORDER BY n.ord
                   ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING) AS parent_id,
               min(CASE WHEN n.lvl <= lv.l THEN n.ord END) OVER (PARTITION BY lv.l ORDER BY n.ord
                   ROWS BETWEEN 1 FOLLOWING AND UNBOUNDED FOLLOWING) AS nxt
             FROM qn n, lv)
  SELECT node_id, ord, lvl, parent_id, coalesce(nxt, (SELECT max(ord) + 1 FROM qn)) AS e
  FROM x WHERE l = lvl;
CREATE TEMP TABLE p AS SELECT * FROM read_ast('$work/tiny.sh') WHERE false;
INSERT INTO p (node_id, type, flags, name, signature_type, modifiers, annotations, file_path, language,
               start_line, end_line, parent_id, depth, sibling_index, children_count, descendant_count, peek)
  SELECT a.node_id::BIGINT, a.ty::VARCHAR, 0::UTINYINT, a.nm::VARCHAR, NULL::VARCHAR, NULL::VARCHAR[],
         NULL::VARCHAR, 'doc', 'duck_block', NULL::UINTEGER, NULL::UINTEGER, e.parent_id::BIGINT,
         a.lvl::UINTEGER, (row_number() OVER (PARTITION BY e.parent_id ORDER BY a.ord) - 1)::INTEGER,
         coalesce(c.n, 0)::UINTEGER, (e.e - a.ord - 1)::UINTEGER, a.nm::VARCHAR
  FROM qn a JOIN qe e ON e.node_id = a.node_id
  LEFT JOIN (SELECT k.parent_id, count(*) AS n FROM qe k JOIN qe pp ON pp.node_id = k.parent_id
             WHERE k.lvl = pp.lvl + 1 GROUP BY k.parent_id) c ON c.parent_id = a.node_id
  ORDER BY a.node_id;
SQL

cases="$here/ast_select_cases.txt"
n=0; bad=0
while IFS= read -r sel; do
  [ -z "$sel" ] && continue
  case "$sel" in \#\ *) continue;; esac
  n=$((n+1))
  q=$(printf '%s' "$sel" | sed "s/'/''/g")
  out=$("$duckdb" -no-agent -unsigned -list -noheader \
    -c ".read $work/setup.sql" -c ".read $work/doc.sql" -c ".read $work/proj.sql" \
    -c "WITH d AS (SELECT unnest(dcs_select(b, '$q')) AS o FROM blocks),
             a AS (SELECT node_id - 2 AS o FROM ast_select_from('p', '$q') WHERE node_id > 1)
        SELECT 'dmb_only=' || coalesce((SELECT string_agg(o::VARCHAR, ',' ORDER BY o) FROM (FROM d EXCEPT FROM a)), '') ||
               ' ast_only=' || coalesce((SELECT string_agg(o::VARCHAR, ',' ORDER BY o) FROM (FROM a EXCEPT FROM d)), '') ||
               ' n=' || (SELECT count(*) FROM d);" 2>&1 | tail -1)
  case "$out" in
    "dmb_only= ast_only= n="*) echo "ok   [$sel] ${out##* }";;
    *) echo "DIFF [$sel] $out"; bad=$((bad+1));;
  esac
done < "$cases"
if [ "$n" -eq 0 ]; then echo "FAIL: no cases ran"; exit 1; fi
if [ "$bad" -ne 0 ]; then echo "FAIL: $bad of $n selectors differ"; exit 1; fi
echo "OK: $n selectors, dmb and ast_select_from agree"
