#!/usr/bin/env bash
# The D9 wrappers against the real readers. Not part of `make test`: the unittest binary can't
# load other extensions. Every extension must be built from the same DuckDB commit as dmb.
#   MARKDOWN_EXT=.../markdown.duckdb_extension WEBBED_EXT=.../webbed.duckdb_extension \
#     test/integration/readers.sh [path/to/duckdb] [path/to/dmb.duckdb_extension]
set -euo pipefail
duckdb=${1:-build/release/duckdb}
ext=${2:-build/release/extension/dmb/dmb.duckdb_extension}
: "${MARKDOWN_EXT:?set MARKDOWN_EXT}" "${WEBBED_EXT:?set WEBBED_EXT}"
fail=0
check() { # name, sql, expected
  local got
  got=$( { "$duckdb" -no-agent -unsigned -list -noheader -c "LOAD '$ext'; LOAD '$MARKDOWN_EXT'; LOAD '$WEBBED_EXT'; $2" 2>&1 || true; } | tail -1)
  if [ "$got" == "$3" ]; then echo "ok   $1"; else echo "FAIL $1"; echo "     want: $3"; echo "     got:  $got"; fail=1; fi
}
doc="[{'kind': 'block', 'element_type': 'heading', 'content': 'Comments', 'level': 1, 'encoding': 'text', 'attributes': MAP {'heading_level': '2', 'id': 'comments'}, 'element_order': 0}, {'kind': 'block', 'element_type': 'paragraph', 'content': '', 'level': 1, 'encoding': 'text', 'attributes': MAP {}, 'element_order': 1}, {'kind': 'inline', 'element_type': 'text', 'content': 'Hi', 'level': 2, 'encoding': 'text', 'attributes': MAP {}, 'element_order': 2}]"
show="lambda x: x.element_order || ':' || x.kind || ':' || x.element_type || ':' || coalesce(x.content, '') || ':' || x.level"

# The spec's first-use sheet, parsed: the anchor becomes a real link inside the heading. The
# leading space, which the markdown reader drops, is kept as its own text row.
check "inline markdown anchor" \
  "SELECT '[' || array_to_string(list_transform(dcs_parse_markdown(dcs_apply($doc, 'h2::after { content: \" [¶](#\" attr(id) \")\"; -dcs-content-as: markdown; }')), $show), ', ') || ']';" \
  "[0:block:heading:Comments:1, 1:inline:text:Comments:2, 2:inline:text: :2, 3:inline:link:¶:2, 4:block:paragraph::1, 5:inline:text:Hi:2]"

check "link href kept" \
  "SELECT dcs_parse_markdown(dcs_apply($doc, 'h2::after { content: \"[¶](#\" attr(id) \")\"; -dcs-content-as: markdown; }'))[3].attributes['href'];" \
  "#comments"

# Block placement: the parsed paragraph and its inlines, at the block's level.
check "block markdown" \
  "SELECT '[' || array_to_string(list_transform(dcs_parse_markdown(dcs_apply($doc, 'p::after { content: \"**Note**\"; -dcs-content-as: md; -dcs-content-place: block; }')), $show)[4:], ', ') || ']';" \
  "[3:block:paragraph::1, 4:inline:bold:Note:2]"

check "inline html" \
  "SELECT '[' || array_to_string(list_transform(dcs_parse_html(dcs_apply($doc, 'p::after { content: \"<b>x</b>\"; -dcs-content-as: html; }')), $show)[3:], ', ') || ']';" \
  "[2:inline:text:Hi:2, 3:inline:bold:x:2]"

# A wrapper leaves rows marked for another format alone, and a document with no marks unchanged.
check "html marks untouched by markdown wrapper" \
  "SELECT dcs_parse_markdown(dcs_apply($doc, 'p::after { content: \"<b>x</b>\"; -dcs-content-as: html; }'))[4].element_type;" \
  "raw"
check "no marks, no change" "SELECT dcs_parse_markdown($doc) = $doc;" "true"

[ "$fail" -eq 0 ] && echo "OK: 6 checks" || { echo "FAILED"; exit 1; }
