# Function reference

In these functions, `blocks` is any list of structs that has the seven duck_block fields:
`kind`, `element_type`, `content`, `level`, `encoding`, `attributes MAP(VARCHAR, VARCHAR)` and
`element_order`. Other fields pass through unchanged. A missing field, or a field of the wrong
type, gives a Binder Error that names the field.

| Function | Kind | Returns |
|---|---|---|
| [`dcs_apply`](#dcs_apply) | scalar | the document with the sheet applied |
| [`dcs_select`](#dcs_select) | scalar | the blocks that a selector matches |
| [`dcs_explain`](#dcs_explain) | scalar | one entry per change, with the rule that made it |
| [`dcs_check`](#dcs_check) | table | the diagnostics for a sheet |
| [`dcs_rules`](#dcs_rules) | table | the rules and declarations of a sheet |
| [`dcs_sheet`](#dcs_sheet) | scalar | a sheet, parsed into a [`DCS`](#the-dcs-type) value |
| [`dcs_format`](#dcs_format) | scalar | a [`DCS`](#the-dcs-type) value, written as sheet text |
| [`read_dcs_objects`](#read_dcs_objects-read_dcs) | table | one row per `.dcs` file, with its sheet and diagnostics |
| [`read_dcs`](#read_dcs_objects-read_dcs) | table | one row per rule of each `.dcs` file |
| [`dcs_parse_markdown`, `dcs_parse_html`](#dcs_parse_markdown-dcs_parse_html) | macro | the document, with marked content parsed by a reader |
| [`dcs_version`](#dcs_version-dcs_parser_abi) | scalar | the dmb version |
| [`dcs_parser_abi`](#dcs_version-dcs_parser_abi) | scalar | the tree-sitter-css ABI |

`slug()`, `text()`, `attr()`, `counter()` and `url()` are not SQL functions. They are DCS value
functions, and you use them inside a sheet. See [values](language/cascade.md#values).

## dcs_apply

```text
dcs_apply(blocks, sheet VARCHAR or DCS) -> same type as blocks
dcs_apply(blocks, sheet VARCHAR or DCS, strict BOOLEAN) -> same type as blocks
```

Applies the sheet and returns the new document. The sheet can be different on each row.

The sheet is text or a [`DCS`](#the-dcs-type) value: any list is read as a `DCS` value, and anything
else as text. A `DCS` value gives the same document as its text. In strict mode, a diagnostic that
belongs to a rule (its selector, a property or a value) gives the same error for both. A text-level
diagnostic stays with the text: CSS that does not parse, an `@import` or another at-rule, or a nested
rule. `dcs_sheet` drops it, so strict mode raises it only for the text.

A problem in the sheet never stops the document: dmb skips the rule or declaration and continues.
Give `true` as the third argument to raise the first diagnostic as an error. In the scalar form the
third argument is positional: `strict := true` does not bind. The [table form](#the-table-form)
takes either.

```sql
SELECT dcs_apply(doc, 'p { text-overflow: ellipsis; }', true) FROM docs;
```

```text
Invalid Input Error: dcs_apply: not-yet at line 1, column 5: text-overflow is not supported yet
```

Without `true`, the same sheet keeps all 6 rows and gives no error.

Some diagnostics need a document. An example is `-dcs-level` on a block that is not a heading.
These come only from `dcs_apply`, and only in strict mode.

### The table form

```text
FROM dcs_apply((subquery), sheet VARCHAR or DCS [, strict BOOLEAN])
```

Takes block rows directly, for example from a reader, with no `list()` around them:

```sql
FROM dcs_apply((FROM read_markdown_blocks('posts/*.md')), 'heading { -dcs-id: auto; }');
```

- The input is block rows: the seven duck_block fields, and any other columns. The output has the
  input's columns, in the same order. Other columns pass through unchanged.
- The rows are grouped into documents by the `filename` column. With no `filename` column, the whole
  input is one document. In a document, the rows are taken in `element_order` order.
- The output for each document is the scalar `dcs_apply` output, one row per block. The documents come
  out in `filename` order, and rows with a NULL `filename` come last as one document.
- The sheet must be a constant. `strict` can be given by position or as `strict := true`.
- The function holds its whole input in memory. See [limits](limits.md#performance-on-large-documents).

## dcs_select

```text
dcs_select(blocks, selector VARCHAR) -> INTEGER[]
```

Returns the `element_order` of each block that the selector matches, in document order.

```sql
SELECT dcs_select(doc, ':section(h2:contains("Comments"))') AS section_rows,
       dcs_select(doc, 'heading') AS headings,
       dcs_select(doc, 'p') AS paragraphs
FROM docs;
```

| section_rows | headings | paragraphs |
|--------------|----------|------------|
| [3, 4]       | [0, 3]   | [1, 4]     |

## dcs_explain

```text
dcs_explain(blocks, sheet VARCHAR or DCS)
  -> STRUCT(element_order, rule, selector, property, before, after, line, "column")[]
```

Returns one entry for each change that the sheet makes. Each entry names the winning rule, and the
line and column of the winning declaration.

```sql
SELECT e.* FROM (SELECT unnest(dcs_explain(doc, 'heading { -dcs-id: auto; }
.comment { --audience: "kids"; }')) AS e FROM docs);
```

| element_order | rule | selector | property   | before | after       | line | column |
|--------------:|-----:|----------|------------|--------|-------------|-----:|-------:|
| 0             | 0    | heading  | -dcs-id    | NULL   | field-guide | 1    | 11     |
| 3             | 0    | heading  | -dcs-id    | NULL   | comments    | 1    | 11     |
| 4             | 1    | .comment | --audience | NULL   | kids        | 2    | 12     |

A change from `@frontmatter` has `rule` NULL and `selector` `@frontmatter`.

## dcs_check

```text
dcs_check(sheet VARCHAR) -> TABLE(severity, code, message, line, column)
```

Returns one row for each diagnostic in the sheet. It needs no document.

```sql
SELECT * FROM dcs_check('h2 { -dcs-level: up; } p { --x: frob(); }');
```

| severity | code             | message                              | line | column |
|----------|------------------|--------------------------------------|-----:|-------:|
| error    | bad-value        | -dcs-level takes +n, -n or n, not up | 1    | 6      |
| error    | unknown-function | --x: unknown-function: frob()        | 1    | 28     |

See [diagnostics](diagnostics.md) for the codes.

## dcs_rules

```text
dcs_rules(sheet VARCHAR) -> TABLE(rule, selector, pseudo_element, property, value, important,
                                  specificity, order, media, line, column)
```

Returns one row for each rule and declaration. The sheet must be a constant.

A rule whose selector was refused keeps its whole selector text, and its `pseudo_element` is NULL:
`p::before.x` gives the selector `p::before.x`, not `p` with `before`.

```sql
SELECT * FROM dcs_rules('h1, h2 { -dcs-id: auto; }
p.comment { color: red !important; }
@frontmatter { title: text(h1); }');
```

| rule | selector     | pseudo_element | property | value    | important | specificity | order | media | line | column |
|-----:|--------------|----------------|----------|----------|-----------|-------------|------:|-------|-----:|-------:|
| 0    | h1           | NULL           | -dcs-id  | auto     | false     | [0, 0, 1]   | 0     | NULL  | 1    | 10     |
| 1    | h2           | NULL           | -dcs-id  | auto     | false     | [0, 0, 1]   | 0     | NULL  | 1    | 10     |
| 2    | p.comment    | NULL           | color    | red      | true      | [0, 1, 1]   | 1     | NULL  | 2    | 13     |
| NULL | @frontmatter | NULL           | title    | text(h1) | false     | [0, 0, 0]   | 2     | NULL  | 3    | 16     |

The selector list `h1, h2` gives two rules with the same `order`. `@frontmatter` declarations
have `rule` NULL. A rule inside `@media (format: X)` has `X` in the `media` column.

## dcs_parse_markdown, dcs_parse_html

```text
dcs_parse_markdown(blocks) -> blocks
dcs_parse_html(blocks) -> blocks
```

These are macros. They find the generated content that a sheet marked with
`-dcs-content-as: markdown` (or `md`) or `-dcs-content-as: html`. They replace each marked row
with the rows that the reader makes from it. `dcs_parse_markdown` needs the markdown extension,
and `dcs_parse_html` needs the webbed extension. Each macro leaves rows marked for another format
unchanged. A document with no marked rows comes back unchanged.

The [¶ anchor sheet](cookbook.md#a-anchor-after-every-heading) shows `dcs_parse_markdown`.

## The DCS type

A `DCS` value is a parsed sheet. dmb's functions use it to hand a sheet to each other, and you can
store it in a table or a Parquet file, or build one in SQL.

```text
DCS = STRUCT(selector     VARCHAR,
             meta         VARCHAR,
             declarations STRUCT(property VARCHAR, value VARCHAR, extra JSON)[],
             extra        JSON)[]
```

One list entry is one rule after selector lists are split: `h2, h3 { ... }` gives two entries with
the same declarations. The entries are in source order, and so are the declarations of each entry.

| Field | Holds |
|---|---|
| `selector` | The selector text of one rule, without the pseudo-element. NULL for the `@frontmatter` entry |
| `meta` | NULL for a plain rule. `'frontmatter'` for the declarations of `@frontmatter` |
| `declarations` | In source order. `value` is the source text of the value |
| rule `extra` | `pseudo_element` (`"before"` or `"after"`), `media` (the X of `@media (format: X)`), `line`, `column` |
| declaration `extra` | `important` (only when true), `line`, `column` |

- A key with no value is left out of `extra`, and `extra` is NULL when it is empty.
- The `@frontmatter` entry, if there is one, is the last entry. Two `@frontmatter` blocks in the
  text become one entry.
- A rule whose selector was refused stays in the value, so rule numbers are the same as for the text.
- Comments, whitespace and text-level diagnostics are not kept. `dcs_check` shows those for the text.

`DCS` is registered without a type alias. On DuckDB v2.0.1 an aliased list type does not work
with `len`, `v[i]` or `list_transform`. So `typeof` and `DESCRIBE` show the struct list, not `DCS`.
`CAST(x AS DCS)` and a column declared as `DCS` work.

## dcs_sheet

```text
dcs_sheet(sheet VARCHAR) -> DCS
```

Parses a sheet into a [`DCS`](#the-dcs-type) value. NULL gives NULL.

```sql
SELECT s.selector, s.extra
FROM (SELECT unnest(dcs_sheet('h2, h3::before { content: "#"; }')) AS s);
```

| selector | extra |
|---|---|
| h2 | {"line":1,"column":1} |
| h3 | {"pseudo_element":"before","line":1,"column":5} |

## dcs_format

```text
dcs_format(sheet DCS or VARCHAR) -> VARCHAR
```

Writes a [`DCS`](#the-dcs-type) value as sheet text, one rule per line, in list order. The rules of a
split selector list stay split. Values are written as stored. Rules with a media format go inside
`@media (format: X) { ... }`, one block for each run of rules with the same format. The
`@frontmatter` entry is written as `@frontmatter { ... }`. Text is parsed first, so
`dcs_format(text)` gives the text in this form. NULL gives NULL.

For every sheet in dmb's tests, `dcs_apply(doc, dcs_format(dcs_sheet(t)))` gives the same document
as `dcs_apply(doc, t)`. Comments and whitespace are not kept, so `dcs_explain` gives other lines and
columns for the formatted text.

```sql
SELECT dcs_format(dcs_sheet('h2, h3 { color: red } p::before { content: "x" !important }'));
```

```text
h2 { color: red; }
h3 { color: red; }
p::before { content: "x" !important; }
```

## read_dcs_objects, read_dcs

```text
read_dcs_objects(globs VARCHAR or VARCHAR[])
  -> TABLE(filename VARCHAR, content VARCHAR, sheet DCS,
           diagnostics STRUCT(severity, code, message, line, "column")[])
read_dcs(globs VARCHAR or VARCHAR[])
  -> TABLE(filename VARCHAR, rule INTEGER, selector VARCHAR, meta VARCHAR,
           declarations STRUCT(property, value, extra JSON)[], extra JSON)
```

`read_dcs_objects` gives one row per file, in filename order. `sheet` is `dcs_sheet(content)`, and
`diagnostics` holds the rows that `dcs_check(content)` gives, the text-level ones included. An
empty file is a row with an empty sheet.

`read_dcs` gives one row per entry of each file's [`DCS`](#the-dcs-type) value. `rule` is the
entry's 0-based position, which is the rule number of `dcs_rules` and `dcs_explain` for a plain
rule. It has no diagnostics column: use `read_dcs_objects` or `dcs_check`.

A glob that matches no file, or a file that cannot be read, is an error.

```sql
SELECT filename, dcs_apply(doc, sheet) AS doc
FROM docs, read_dcs_objects('styles/*.dcs');
```

## dcs_version, dcs_parser_abi

```sql
SELECT dcs_version() AS version, dcs_parser_abi() AS parser_abi;
```

| version | parser_abi |
|---------|-----------:|
| 0.1.0   | 15         |

`dcs_parser_abi` is the tree-sitter language ABI of the CSS grammar that dmb was built with.
