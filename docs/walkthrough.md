# Walkthrough

This page follows one task from start to end. A team keeps field notes in a Markdown file. The
public site must get a cleaned copy of it. The sheet in this walkthrough does four things:

- it gives every heading an id, so other pages can link to it
- it drops the "Internal" section
- it drops the TODO paragraphs
- it writes the title into the front matter

Each step shows one query and the result that dmb gave for it.

## 0. Set up

The walkthrough uses DuckDB v2.0.1 and three extensions. The markdown extension reads the file,
dmb applies the sheet, and duck_block_utils writes the result as Markdown.

```sql
INSTALL markdown FROM community;
INSTALL duck_block_utils FROM community;
LOAD markdown;
LOAD duck_block_utils;
```

!!! note "dmb is not in the community extensions yet"
    Until it is, build dmb from source with the DuckDB version set, so that it loads beside the
    community extensions:

    ```sh
    OVERRIDE_GIT_DESCRIBE=v2.0.1 make release GEN=ninja
    build/release/duckdb -unsigned
    ```

    ```sql
    LOAD 'build/release/extension/dmb/dmb.duckdb_extension';
    ```

## 1. The document

The team's notes are in `notes.md`:

```markdown
# Duck field notes

Notes from the spring count.

## Internal

Counts are not checked yet.

## Mallards

TODO: add photos

We saw 14 mallards at the north pond.
```

`read_markdown_blocks` gives one row per block. The dmb functions take a document as one list, so
put the rows in a list, in `element_order`:

```sql
CREATE TABLE notes AS
  SELECT list(b ORDER BY b.element_order) AS doc
  FROM read_markdown_blocks('notes.md') AS b;

SELECT u.element_order, u.element_type, u.content, u.attributes
FROM (SELECT unnest(doc) AS u FROM notes);
```

| element_order | element_type | content                               | attributes                               |
|--------------:|--------------|---------------------------------------|------------------------------------------|
| 0             | heading      | Duck field notes                      | `{heading_level=1, id=duck-field-notes}` |
| 1             | paragraph    | Notes from the spring count.          | `{}`                                     |
| 2             | heading      | Internal                              | `{heading_level=2, id=internal}`         |
| 3             | paragraph    | Counts are not checked yet.           | `{}`                                     |
| 4             | heading      | Mallards                              | `{heading_level=2, id=mallards}`         |
| 5             | paragraph    | TODO: add photos                      | `{}`                                     |
| 6             | paragraph    | We saw 14 mallards at the north pond. | `{}`                                     |

## 2. Write the sheet

Put the sheet in a file, `public.dcs`. Each rule does one of the four jobs:

```css
/* public.dcs: turn the team's field notes into the public page */
heading { -dcs-id: auto; }
h2:contains("Internal") { -dcs-prune: section; }
p:contains("TODO") { display: none; }
@frontmatter { title: text(h1); }
```

- `-dcs-id: auto` makes an id from the heading text. It keeps an id that the heading has already.
- `-dcs-prune: section` drops the heading and its section, up to the next heading of the same
  or a higher rank.
- `display: none` drops the block and all the blocks under it.
- `@frontmatter` writes keys into the front matter. `text(h1)` is the text of the first `h1`.

## 3. Check the sheet

A mistake in a sheet never stops a document. dmb records it as a diagnostic and skips the
declaration. So check a sheet before you use it. `dcs_check` needs no document:

```sql
SELECT severity, code, message, line, "column"
FROM dcs_check('h2:contains("Internal") { -dcs-prun: section; }');
```

| severity | code             | message                    | line | column |
|----------|------------------|----------------------------|-----:|-------:|
| error    | unknown-property | unknown property -dcs-prun | 1    | 27     |

The property name has a typo. Without the check, the "Internal" section stays in the public copy.

`read_dcs_objects` reads sheet files. It gives one row per file, with the diagnostics of that file:

```sql
SELECT filename, len(diagnostics) AS problems FROM read_dcs_objects('public.dcs');
```

| filename   | problems |
|------------|---------:|
| public.dcs | 0        |

The [diagnostics](diagnostics.md) page lists every code.

## 4. Preview the selectors

`dcs_select` shows the `element_order` of each block that a selector matches. Use it to make sure
that a selector chooses the blocks you expect, before you add declarations:

```sql
SELECT dcs_select(doc, 'heading') AS headings,
       dcs_select(doc, 'h2:contains("Internal")') AS internal,
       dcs_select(doc, 'p:contains("TODO")') AS todos
FROM notes;
```

| headings  | internal | todos |
|-----------|----------|-------|
| [0, 2, 4] | [2]      | [5]   |

## 5. Apply the sheet

`dcs_apply` takes the document and the sheet, and returns the new document. Here the sheet comes
from the file:

```sql
SELECT u.element_order, u.element_type, u.content, u.attributes
FROM (SELECT unnest(dcs_apply(doc, sheet)) AS u
      FROM notes, read_dcs_objects('public.dcs'));
```

| element_order | element_type | content                               | attributes                               |
|--------------:|--------------|---------------------------------------|------------------------------------------|
| 0             | metadata     | `title: "Duck field notes"`           | `{role=frontmatter}`                     |
| 1             | heading      | Duck field notes                      | `{heading_level=1, id=duck-field-notes}` |
| 2             | paragraph    | Notes from the spring count.          | `{}`                                     |
| 3             | heading      | Mallards                              | `{heading_level=2, id=mallards}`         |
| 4             | paragraph    | We saw 14 mallards at the north pond. | `{}`                                     |

The result has these changes:

- A front matter block is at the start, because the file had none.
- The "Internal" section and the TODO paragraph are gone.
- dmb numbers `element_order` again from 0.

## 6. Explain the changes

`dcs_explain` gives one entry for each change. Each entry names the rule that won, and the line of
its declaration in the sheet. `element_order` is the position in the input document:

```sql
SELECT e.element_order, e.selector, e.property, e.before, e.after, e.line
FROM (SELECT unnest(dcs_explain(doc, sheet)) AS e
      FROM notes, read_dcs_objects('public.dcs'));
```

| element_order | selector                | property           | before  | after            | line |
|--------------:|-------------------------|--------------------|---------|------------------|-----:|
| 2             | h2:contains("Internal") | -dcs-prune         | present | NULL             | 3    |
| 5             | p:contains("TODO")      | display            | present | NULL             | 4    |
| 0             | @frontmatter            | @frontmatter title | NULL    | Duck field notes | 5    |

The heading rule (line 2) is not in the list. The markdown reader gave each heading an id
already, and `-dcs-id: auto` keeps an id that exists. So that rule changed nothing in this
document. It matters for a document from a reader that gives no ids.

## 7. Write the result

`duck_blocks_to_md` in duck_block_utils writes the new document as Markdown:

```sql
SELECT duck_blocks_to_md(dcs_apply(doc, sheet)) AS md
FROM notes, read_dcs_objects('public.dcs');
```

```markdown
---
title: "Duck field notes"

---

# Duck field notes

Notes from the spring count.

## Mallards

We saw 14 mallards at the north pond.
```

This is the output as it is today, with dmb 0.1.0 and duck_block_utils v4.1.0. The front matter
has an empty line before its closing `---`.

## 8. Look at the stored sheet

A sheet is also a value of the [`DCS` type](reference.md#the-dcs-type). `read_dcs` gives one row
per rule of each file:

```sql
SELECT rule, selector, meta, declarations FROM read_dcs('public.dcs');
```

| rule | selector                | meta        | declarations                                                                    |
|-----:|-------------------------|-------------|---------------------------------------------------------------------------------|
| 0    | heading                 | NULL        | `[{'property': -dcs-id, 'value': auto, 'extra': '{"line":2,"column":11}'}]`       |
| 1    | h2:contains("Internal") | NULL        | `[{'property': -dcs-prune, 'value': section, 'extra': '{"line":3,"column":27}'}]` |
| 2    | p:contains("TODO")      | NULL        | `[{'property': display, 'value': none, 'extra': '{"line":4,"column":22}'}]`       |
| 3    | NULL                    | frontmatter | `[{'property': title, 'value': 'text(h1)', 'extra': '{"line":5,"column":16}'}]`   |

`dcs_format` writes a `DCS` value as sheet text again, one rule per line. It does not keep
comments:

```sql
SELECT dcs_format(sheet) AS formatted FROM read_dcs_objects('public.dcs');
```

```css
heading { -dcs-id: auto; }
h2:contains("Internal") { -dcs-prune: section; }
p:contains("TODO") { display: none; }
@frontmatter { title: text(h1); }
```

## Next

- [Selectors](language/selectors.md) lists every selector and pseudo-class.
- [Properties by phase](language/properties.md) lists what a declaration can change.
- The [cookbook](cookbook.md) has more sheets: reader mode, heading anchors, id prefixes.
- The [table form of `dcs_apply`](reference.md#the-table-form) takes reader rows with no
  `list()`, for many files at once.
