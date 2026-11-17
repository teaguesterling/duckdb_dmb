# Cookbook

These recipes use the `docs` table from [getting started](getting-started.md), unless they make
their own document.

## Reader mode

Reader mode keeps the article and drops the page around it. This document is a web page with a
navigation bar, a main section and a footer:

```sql
CREATE TABLE page AS SELECT [
  blk('section',   '',                 1, MAP {'role': 'nav'},        0),
  blk('paragraph', '',                 2, MAP {},                     1),
  blk('link',      'Home',             3, MAP {'href': '/'},          2),
  blk('section',   '',                 1, MAP {'role': 'main'},       3),
  blk('heading',   'Field guide',      2, MAP {'heading_level': '1'}, 4),
  blk('paragraph', '',                 2, MAP {},                     5),
  blk('text',      'Ducks are birds.', 3, MAP {},                     6),
  blk('paragraph', '',                 2, MAP {},                     7),
  blk('link',      'Share',            3, MAP {'href': '/share'},     8),
  blk('section',   '',                 1, MAP {'role': 'footer'},     9),
  blk('paragraph', '',                 2, MAP {},                     10),
  blk('text',      '© 2026',           3, MAP {},                     11)
] AS doc;

SELECT dcs_select(doc, ':chrome') AS chrome, dcs_select(doc, ':main') AS main,
       dcs_select(doc, ':link-only') AS link_only
FROM page;
```

| chrome       | main | link_only |
|--------------|------|-----------|
| [0, 1, 7, 9] | [3]  | [1, 7]    |

`:chrome` matches the `nav` and `footer` sections, and also each paragraph that holds only a link.
Drop the chrome and the metadata, and unwrap the main section:

```css
:chrome, :meta { display: none; }
:main { display: contents; }
```

```sql
SELECT u.element_order, u.element_type, u.content, u.level
FROM (SELECT unnest(dcs_apply(doc, ':chrome, :meta { display: none; } :main { display: contents; }')) AS u FROM page);
```

| element_order | element_type | content          | level |
|--------------:|--------------|------------------|------:|
| 0             | heading      | Field guide      | 1     |
| 1             | paragraph    |                  | 1     |
| 2             | text         | Ducks are birds. | 2     |

The "Share" paragraph is gone too, because it holds only a link.

## A ¶ anchor after every heading

This sheet gives each `h2` an id, and adds a Markdown link to that id after the heading's text:

```css
h2 { -dcs-id: auto; }
h2::after {
  content: " [¶](#" attr(id) ")";
  -dcs-content-as: markdown;
}
```

`dcs_apply` adds the link as a `raw` row, marked as Markdown:

```sql
SELECT u.element_order, u.kind, u.element_type, u.content, u.attributes
FROM (SELECT unnest(dcs_apply(doc, 'h2 { -dcs-id: auto; }
h2::after { content: " [¶](#" attr(id) ")"; -dcs-content-as: markdown; }')) AS u FROM docs)
WHERE u.element_order BETWEEN 3 AND 5;
```

| element_order | kind   | element_type | content            | attributes                                   |
|--------------:|--------|--------------|--------------------|----------------------------------------------|
| 3             | block  | heading      | Comments           | `{heading_level=2, id=comments}`             |
| 4             | inline | text         | Comments           | `{}`                                         |
| 5             | inline | raw          | `" [¶](#comments)"` | `{dcs-content-as=markdown, format=markdown}` |

`dcs_parse_markdown` parses the marked row with the markdown extension's reader. The link becomes
a real `link` row:

```sql
SELECT u.element_order, u.kind, u.element_type, u.content, u.attributes
FROM (SELECT unnest(dcs_parse_markdown(dcs_apply(doc, 'h2 { -dcs-id: auto; }
h2::after { content: " [¶](#" attr(id) ")"; -dcs-content-as: markdown; }'))) AS u FROM docs)
WHERE u.element_order BETWEEN 3 AND 7;
```

| element_order | kind   | element_type | content  | attributes                       |
|--------------:|--------|--------------|----------|----------------------------------|
| 3             | block  | heading      | Comments | `{heading_level=2, id=comments}` |
| 4             | inline | text         | Comments | `{}`                             |
| 5             | inline | text         | `" "`    | `{}`                             |
| 6             | inline | link         | ¶        | `{href=#comments}`               |
| 7             | block  | paragraph    |          | `{class=comment}`                |

The markdown reader drops a leading space, so dmb keeps the space as its own `text` row (row 5).
This run loaded a markdown extension built from the same DuckDB commit as dmb.

## Rename ids

Build each heading's id from a prefix and the slug of its text:

```sql
SELECT u.element_order, u.attributes['id'] AS id
FROM (SELECT unnest(dcs_apply(doc, 'heading { -dcs-id: "sec-" slug(text()); }')) AS u FROM docs)
WHERE u.element_type = 'heading';
```

| element_order | id              |
|--------------:|-----------------|
| 0             | sec-field-guide |
| 3             | sec-comments    |

A value replaces the id that a reader set. `-dcs-id: auto` does not: it keeps an existing id, and
it makes the new ids unique.

```sql
SELECT u.element_order, u.attributes['id'] AS id
FROM (SELECT unnest(dcs_apply([
  blk('heading', 'Notes', 1, MAP {'heading_level': '2', 'id': 'keep-me'}, 0),
  blk('heading', 'Notes', 1, MAP {'heading_level': '2'}, 1),
  blk('heading', 'Notes', 1, MAP {'heading_level': '2'}, 2)
], 'heading { -dcs-id: auto; }')) AS u);
```

| element_order | id      |
|--------------:|---------|
| 0             | keep-me |
| 1             | notes   |
| 2             | notes-1 |

## Prune chrome

Drop whole sections by their heading text:

```css
h2:contains("Comments") { -dcs-prune: section; }
```

Drop paragraphs that hold only a link, and paragraphs that repeat earlier text:

```sql
SELECT u.element_order, u.element_type, u.content
FROM (SELECT unnest(dcs_apply([
  blk('paragraph', '', 1, MAP {}, 0),               blk('text', 'Read more', 2, MAP {}, 1),
  blk('paragraph', '', 1, MAP {}, 2),               blk('link', 'Subscribe', 2, MAP {'href': '/sub'}, 3),
  blk('paragraph', '', 1, MAP {}, 4),               blk('text', 'Read more', 2, MAP {}, 5)
], 'p { -dcs-dedupe: text; } :link-only { display: none; }')) AS u);
```

| element_order | element_type | content   |
|--------------:|--------------|-----------|
| 0             | paragraph    |           |
| 1             | text         | Read more |

The `-dcs-dedupe` rule drops the second "Read more" paragraph. The `:link-only` rule drops the
"Subscribe" paragraph.
