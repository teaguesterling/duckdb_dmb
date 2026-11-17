# Getting started

## Build

dmb builds with the standard DuckDB extension template. You need git, CMake, a C++17 compiler and
ninja.

```sh
git clone --recurse-submodules https://github.com/teaguesterling/duckdb_dmb
cd duckdb_dmb
make release GEN=ninja
build/release/test/unittest "test/sql/*"
```

The build makes a DuckDB CLI with dmb in it (`build/release/duckdb`) and a loadable extension
file (`build/release/extension/dmb/dmb.duckdb_extension`).

## Load

The extension file is not signed. Start the CLI with `-unsigned`, then load the file:

```sh
build/release/duckdb -unsigned
```

```sql
LOAD 'build/release/extension/dmb/dmb.duckdb_extension';
SELECT dcs_version() AS version, dcs_parser_abi() AS parser_abi;
```

| version | parser_abi |
|---------|-----------:|
| 0.1.0   | 15         |

!!! note "A local build loads only in its own CLI"
    A local build reports the DuckDB commit (`d54cbbe3df`) as its version, not `v2.0.0`. So it
    loads only in `build/release/duckdb`. That CLI cannot load extensions installed for
    `v2.0.0`, such as the markdown and webbed readers. The examples on this site make documents
    as literal lists for that reason.

## Make a document

A document is a list of structs with the seven duck_block fields. This macro makes one row. Rows
of type `text`, `link` and `image` are inline rows.

```sql
CREATE MACRO blk(t, c, lvl, attrs, o) AS
  {'kind': CASE WHEN t IN ('text', 'link', 'image') THEN 'inline' ELSE 'block' END,
   'element_type': t, 'content': c, 'level': lvl, 'encoding': 'text',
   'attributes': attrs::MAP(VARCHAR, VARCHAR), 'element_order': o};

CREATE TABLE docs AS SELECT [
  blk('heading',   'Field guide',      1, MAP {'heading_level': '1'}, 0),
  blk('paragraph', '',                 1, MAP {},                     1),
  blk('text',      'Ducks are birds.', 2, MAP {},                     2),
  blk('heading',   'Comments',         1, MAP {'heading_level': '2'}, 3),
  blk('paragraph', '',                 1, MAP {'class': 'comment'},   4),
  blk('text',      'Nice post!',       2, MAP {},                     5)
] AS doc;
```

Most examples on this site use this `docs` table.

A block's parent is the nearest earlier row with a smaller `level`. So row 2 is a child of row 1,
and rows 0 and 1 are both top-level blocks.

## Apply your first sheet

`dcs_apply` takes the document and the sheet. It returns the new document, in the same type as
the input.

```sql
SELECT u.element_order, u.element_type, u.attributes
FROM (SELECT unnest(dcs_apply(doc, 'heading { -dcs-id: auto; }')) AS u FROM docs)
WHERE u.element_type = 'heading';
```

| element_order | element_type | attributes                          |
|--------------:|--------------|-------------------------------------|
| 0             | heading      | `{heading_level=1, id=field-guide}` |
| 3             | heading      | `{heading_level=2, id=comments}`    |

The `-dcs-id: auto` declaration gives each heading an id made from its text.

Next, read about [selectors](language/selectors.md), or look at the [cookbook](cookbook.md).

## With real readers

In a release build, the duck_blocks readers make the document for you. For example,
`read_markdown_blocks` in the markdown extension and `read_html_blocks` in the webbed extension.
Any list of structs with the seven fields works. Other fields, such as `filename`, pass through
unchanged.
