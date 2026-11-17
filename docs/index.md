# dmb

dmb is a DuckDB extension that runs **DCS** sheets. DCS means Cascading Duck Sheets. A DCS sheet
(a `.dcs` file) is a set of CSS-shaped rules. dmb applies a sheet to a duck_blocks document and
returns a new document. Any duck_block_utils renderer can then write the result as Markdown, HTML
or text.

A selector chooses blocks, as in CSS. The declarations change those blocks: they set ids and
attributes, drop sections, wrap or retype blocks, add generated text, reorder blocks, and write
front matter.

!!! warning "Status: early, not released"
    dmb is not in the DuckDB community extensions yet. You build it from source. It targets
    DuckDB v2.0.0. The [limits](limits.md) page lists what does not work yet.

## Before and after

This document has two headings and two paragraphs. The paragraph text is in inline rows
(`kind = 'inline'`), as the duck_blocks readers write it.

| element_order | element_type | content          | attributes          |
|--------------:|--------------|------------------|---------------------|
| 0             | heading      | Field guide      | `{heading_level=1}` |
| 1             | paragraph    |                  | `{}`                |
| 2             | text         | Ducks are birds. | `{}`                |
| 3             | heading      | Comments         | `{heading_level=2}` |
| 4             | paragraph    |                  | `{class=comment}`   |
| 5             | text         | Nice post!       | `{}`                |

Apply a sheet with two rules. The first gives every heading an anchor id. The second sets an
attribute on each block with the class `comment`.

```sql
SELECT u.element_order, u.element_type, u.attributes
FROM (SELECT unnest(dcs_apply(doc,
    'heading  { -dcs-id: auto; }
     .comment { --audience: "kids"; }')) AS u FROM docs);
```

| element_order | element_type | attributes                          |
|--------------:|--------------|-------------------------------------|
| 0             | heading      | `{heading_level=1, id=field-guide}` |
| 1             | paragraph    | `{}`                                |
| 2             | text         | `{}`                                |
| 3             | heading      | `{heading_level=2, id=comments}`    |
| 4             | paragraph    | `{audience=kids, class=comment}`    |
| 5             | text         | `{}`                                |

One more rule removes the whole "Comments" section, from its heading to the next heading of the
same or a higher rank:

```sql
SELECT u.element_order, u.element_type, u.content
FROM (SELECT unnest(dcs_apply(doc, 'h2:contains("Comments") { -dcs-prune: section; }')) AS u FROM docs);
```

| element_order | element_type | content          |
|--------------:|--------------|------------------|
| 0             | heading      | Field guide      |
| 1             | paragraph    |                  |
| 2             | text         | Ducks are birds. |

[Getting started](getting-started.md) builds this `docs` table. All the example output on this
site comes from runs against a local build of dmb on DuckDB v2.0.0 (2026-11-16).

## Terms

| Term | Meaning |
|---|---|
| document | One `duck_block[]` list: the rows of one file, in `element_order` |
| block | One row of a document. dmb never matches inline rows (`kind = 'inline'`). Their text belongs to their block |
| sheet | The text of a `.dcs` file |
| rule | One selector and its declarations. A selector list (`h1, h2`) becomes one rule per selector |
| diagnostic | A problem that dmb found in a sheet. A diagnostic never stops a document |
