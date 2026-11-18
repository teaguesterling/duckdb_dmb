# The DCS type, the DCS readers, and the table form of dcs_apply

Status: **draft for Teague's review (2026-11-18).** Agreed in conversation on 2026-11-18. The
parts marked *(Claude's call)* are proposals that Teague has not yet seen in this form.

## Why

dmb is not ready for release (Teague, 2026-11-18). Today a DCS sheet exists only as text: every
`dcs_apply(doc, sheet)` call parses the text again, and a sheet cannot be held, built, or passed
between functions as structured data. Teague asked for three things that fix this:

1. A value type for a parsed sheet. Teague: "The type format is mostly an internal exchange
   representation." It is the form that dmb's own functions hand to each other.
2. File readers: `read_dcs` and `read_dcs_objects`.
3. A table-in, table-out form of `dcs_apply`.

He also asked that a parsed sheet can be **stored**, and that a sheet can be **written in
structured form** (built in SQL, with no CSS text).

A fourth request, a once-over of the docs site, is a separate project with its own design. It is
not in this document.

## Goals

- A registered type `DCS` that holds everything `dcs_apply` uses from a sheet.
- `dcs_apply` and `dcs_explain` accept a `DCS` value wherever they accept sheet text, and give the
  same result for both. (`dcs_select` takes one selector, not a sheet, so it does not change.)
- A sheet can go from text to `DCS` and back to text.
- A `DCS` value can be built by hand in SQL, stored in a table or a Parquet file, and applied.
- Two readers for `.dcs` files.
- A table function that takes block rows from a reader directly, with no `list()` around them.

## Non-goals

- Byte-for-byte round trips. Comments and whitespace are not kept.
- Parse diagnostics inside the `DCS` value. `dcs_check` stays a text function.
- `COPY ... TO 'x.dcs'`. This can come later.
- A faster `dcs_apply`. The type is for exchange, not for speed.
- A release or a community-extensions submission.

## Anti-goals

- **A sheet must never apply differently from its own text.** If `dcs_apply(doc, t)` and
  `dcs_apply(doc, dcs_sheet(t))` can give different documents, the type has failed, whatever else
  works.
  - **Strict mode** (Teague, 2026-11-18): the strict errors match for every diagnostic that
    belongs to a rule (its selector, property or value). Text-level diagnostics stay with the
    text: CSS that does not parse, `@import`, `@font-face` and other at-rules, and nested rules.
    `dcs_sheet` drops them. `read_dcs_objects` shows them in its `diagnostics` column, and
    `dcs_check` shows them for text. So strict `dcs_apply(doc, t)` can throw on a text-level
    diagnostic where strict `dcs_apply(doc, dcs_sheet(t))` does not. That is intended, and a test
    shows it. This is why
  the `MAP(VARCHAR, VARCHAR)` shape was rejected: a map loses declaration order, `!important`,
  repeated properties and positions, and dmb breaks cascade ties by declaration order.
- A hand-built sheet must not skip a check that text gets. A bad selector or value in a built
  sheet gives the same diagnostic code as in text.

## Approaches considered

| | Shape | Trade-off | Outcome |
|---|---|---|---|
| A | A full rule list: one struct per rule, with a list of declarations | Keeps all that `dcs_apply` uses. Needs a converter from the value to dmb's internal `Sheet` | **Chosen by Teague**, with an `extra` JSON field for less common values |
| B | `LIST(STRUCT(selector, rules MAP(VARCHAR, VARCHAR), meta))` | Simplest, but loses order, `!important`, repeats and positions (see the first anti-goal) | Rejected |
| C | A named text type, as webbed's `XML` | Lossless with almost no code, but the contents cannot be queried or built in SQL | Rejected |

## The type

```
DCS = LIST(STRUCT(
  selector     VARCHAR,
  meta         VARCHAR,
  declarations LIST(STRUCT(property VARCHAR, value VARCHAR, extra JSON)),
  extra        JSON))
```

Registered by name, as `loader.RegisterType("DCS", ...)`, the way duckdb_webbed registers `XML`
(`duckdb_webbed/src/xml_types.cpp:119`).

**One list entry is one rule after selector lists are split.** `h2, h3 { ... }` becomes two
entries with the same declarations. This is what `Rule` holds today (`src/include/sheet.hpp:38`)
and what `dcs_rules` shows.

| Field | Holds |
|---|---|
| `selector` | The selector text of one rule, without the pseudo-element. NULL when `meta` is set |
| `meta` | NULL for a plain rule. `'frontmatter'` for the declarations of `@frontmatter` |
| `declarations` | In source order. `value` is the source text of the value (plan decision D4) |
| rule `extra` | `pseudo_element` (`"before"`/`"after"`), `media` (the X of `@media (format: X)`), `line`, `column` |
| declaration `extra` | `important` (true only), `line`, `column` |

- *(Claude's call)* A key is left out of `extra` when it has no value. `extra` is NULL when it
  would be empty. Unknown keys in `extra` are ignored, so later versions can add keys.
- *(Claude's call)* **There is no `order` field.** A rule's place in the list is its source order,
  and so is a declaration's place in its rule. The converter gives the declarations of one split
  selector list the same order, as `Declaration::order` does now ("shared by every selector of a
  list", `sheet.hpp:34`). The entries of a split list are next to each other, so their order
  against other rules does not change.
- `@frontmatter` holds one entry, with `meta = 'frontmatter'`, `selector` NULL, and its
  declarations.
- *(Claude's call, after the builder check)* **A rule whose selector was refused
  (`usable = false`) stays in the value,** with its selector text. The converter refuses it again,
  with the same diagnostic. So the rule numbers that `dcs_explain` reports are the same for text
  and for `DCS`, because a refused rule takes a number today (`src/sheet_parser.cpp:324`).
- *(Claude's call)* The `@frontmatter` entry, if any, is the last entry. Rule numbers count only
  entries with `meta` NULL, so the frontmatter entry does not shift them.

## Functions

| Function | Does |
|---|---|
| `dcs_sheet(text) → DCS` | *(Claude's call for the name)* Parses text into a `DCS` value |
| `dcs_format(DCS) → VARCHAR` | *(Claude's call)* Writes a `DCS` value as `.dcs` text |
| `dcs_apply(blocks, DCS [, strict])` | As today, with a `DCS` value in place of text |
| `dcs_explain(blocks, DCS)` | As today, with a `DCS` value |
| `read_dcs(glob)` | One row per rule |
| `read_dcs_objects(glob)` | One row per file |
| `dcs_apply((subquery), sheet [, strict])` | The table form |

### From a value to a sheet

A `DCS` value comes from `dcs_sheet`, from a table, or from hand-written SQL. So dmb checks it
whenever it converts it into its internal `Sheet`:

- each `selector` is compiled with `CompileSelectorText` (`sheet.hpp:67`);
- each `value` is tokenized, as the parser does now;
- the result goes through the same `CheckSheet` and strict-mode path as text.

The diagnostics use the codes that text gets.

*(Claude's call, after the builder check)* A hand-built value can hold things that no text can
express. The converter refuses each of them with a diagnostic, so that every value it accepts
can be written by `dcs_format` and read back as the same rules:

| Hand-built case | Code |
|---|---|
| a selector with a top-level comma | `selector-list` |
| a pseudo-element in `selector` and also in `extra.pseudo_element` | `pseudo-element-not-last` |
| a `value` that does not tokenize, or that contains a top-level `;` or `}`, or that ends in `!important` | `bad-value` |
| an empty or NULL `property` | `syntax` |
| a `meta` other than NULL and `'frontmatter'`, two frontmatter entries, or a frontmatter entry with `media` | `syntax` |

The builder may add a new code where none of these fits, and lists it in `docs/diagnostics.md`. A position comes from `extra.line` and
`extra.column` when they are there, and is line 0, column 0 when they are not *(Claude's call)*.

### dcs_format

It writes one rule per entry, in list order. The rules of a split list are not joined again. It
writes `@media (format: X) { ... }` around rules that have `media`, and `@frontmatter { ... }` for
the `meta = 'frontmatter'` entry. Values are written as stored.

Round-trip rule, a test (non-strict, and strict for sheets with no text-level diagnostics): for every sheet `t` in the test suite, `dcs_apply(doc, dcs_format(dcs_sheet(t)))`
equals `dcs_apply(doc, t)`.

### The readers

The pair follows duckdb_webbed: `read_xml` gives rows, `read_xml_objects` gives one row per file
with the document as a value.

- `read_dcs_objects(glob)` returns `filename VARCHAR, content VARCHAR, sheet DCS, diagnostics
  LIST(STRUCT(severity, code, message, line, column))`. The `diagnostics` column holds the
  parse diagnostics, the same rows `dcs_check(content)` gives. A file is never skipped
  silently *(Claude's call)*.
- `read_dcs(glob)` returns one row per rule: `filename`, then the fields of one `DCS` entry
  (`selector`, `meta`, `declarations`, `extra`), plus `rule INTEGER` for the entry's position.
  It has no diagnostics column. `read_dcs_objects` or `dcs_check` shows them *(Claude's call)*.
- Both take a glob or a list of globs, like the duck_blocks readers. A file that cannot be read
  is an error, not an empty result.

### The table form of dcs_apply

```sql
FROM dcs_apply((FROM read_markdown_blocks('posts/*.md')), 'heading { -dcs-id: auto; }');
```

- The same name as the scalar function. DuckDB allows a scalar function and a table function
  with one name: `range` and `generate_series` are both (measured, 2026-11-18).
- The input is block rows: the seven duck_block fields, plus any other columns. Other columns
  pass through unchanged, as they do in the scalar form.
- **The rows are grouped into documents by the `filename` column** (agreed with Teague). If there
  is no `filename` column, the whole input is one document. Within a document, the rows are in
  `element_order` order.
- A sheet rule such as `h2 + p` needs the whole document. So the function holds all its input
  rows and writes the output at the end (DuckDB's `in_out_function_final`,
  `duckdb/src/include/duckdb/function/table_function.hpp:490`).
- *(Claude's call, after the builder check)* The output rows are the scalar `dcs_apply` output
  for each document, unnested. **The documents come out in `filename` order**, so the order does
  not depend on how DuckDB split the input across threads. Tests may assert that order.
- The sheet is text or `DCS`, and must be a constant.

## Constraints

- DuckDB v2.0.0. Heavy builds and test runs go through Slurm.
- Change implementation or tests, never both at once. Each new behaviour gets a test that fails
  first.
- *(Claude's call)* If `JSON` is not available to the loadable dmb on a platform, `extra` is
  `VARCHAR` holding JSON text, on every platform, so the type is one type everywhere. Either way,
  the converter accepts `extra` as `JSON` or as `VARCHAR`, because a value can come back from a
  file with either.
- The `JSON` type comes from DuckDB's `json` extension. It is statically linked in dmb's local CLI
  build, and `'{"a":1}'::JSON` works there (measured, 2026-11-18). **Not measured:** that the
  loadable dmb finds `JSON` on every release platform, wasm included. The first task checks this
  before anything is built on it.
- Never register a name that duck_block_utils registers (plan constraint).

## Left to the builder

- **Threads in the table form.** DuckDB can split one input across threads, so rows of one
  document could reach different thread-local states. The builder makes sure that grouping is
  correct with many threads and many files, for example with one global state, and proves it
  with a test that uses `SET threads` above 1 and an input of more than one chunk (2048 rows).
- The exact SQL syntax for the input of a table-in-out function on v2.0.0. Measure it before the
  plan fixes the tests.
- How `dcs_apply` tells a text sheet from a `DCS` value at bind time.
- Whether the readers use DuckDB's multi-file reader or a simple glob loop.
- Memory: the table form holds the whole input. This is acceptable for v1 of the table form, and
  `docs/limits.md` must say so.
