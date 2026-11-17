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
| [`dcs_parse_markdown`, `dcs_parse_html`](#dcs_parse_markdown-dcs_parse_html) | macro | the document, with marked content parsed by a reader |
| [`dcs_version`](#dcs_version-dcs_parser_abi) | scalar | the dmb version |
| [`dcs_parser_abi`](#dcs_version-dcs_parser_abi) | scalar | the tree-sitter-css ABI |

`slug()`, `text()`, `attr()`, `counter()` and `url()` are not SQL functions. They are DCS value
functions, and you use them inside a sheet. See [values](language/cascade.md#values).

## dcs_apply

```text
dcs_apply(blocks, sheet VARCHAR) -> same type as blocks
dcs_apply(blocks, sheet VARCHAR, strict BOOLEAN) -> same type as blocks
```

Applies the sheet and returns the new document. The sheet can be different on each row.

A problem in the sheet never stops the document: dmb skips the rule or declaration and continues.
Give `true` as the third argument to raise the first diagnostic as an error. The third argument is
positional: `strict := true` does not bind.

```sql
SELECT dcs_apply(doc, 'p { text-overflow: ellipsis; }', true) FROM docs;
```

```text
Invalid Input Error: dcs_apply: not-yet at line 1, column 5: text-overflow is not supported yet
```

Without `true`, the same sheet keeps all 6 rows and gives no error.

Some diagnostics need a document. An example is `-dcs-level` on a block that is not a heading.
These come only from `dcs_apply`, and only in strict mode.

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
dcs_explain(blocks, sheet VARCHAR)
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

## dcs_version, dcs_parser_abi

```sql
SELECT dcs_version() AS version, dcs_parser_abi() AS parser_abi;
```

| version | parser_abi |
|---------|-----------:|
| 0.1.0   | 15         |

`dcs_parser_abi` is the tree-sitter language ABI of the CSS grammar that dmb was built with.
