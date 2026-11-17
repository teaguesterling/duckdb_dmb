# Properties by phase

dmb applies a sheet in seven phases, in this order. DCS uses the CSS name for a property where
CSS has one. It uses a `-dcs-` name only where CSS has none.

| Phase | Properties |
|---|---|
| 1 identity | `-dcs-id`, `-dcs-class` |
| 2 prune | `display: none`, `-dcs-prune`, `-dcs-dedupe` |
| 3 structure | `display: contents`, `-dcs-wrap`, `-dcs-as`, `-dcs-level` |
| 4 content | `content`, `-dcs-content-as`, `-dcs-content-place`, `text-transform`, `-dcs-replace`, `counter-reset`, `counter-increment` |
| 5 order | `order` |
| 6 metadata | `@frontmatter { … }` |
| 7 presentation | any other CSS property |

Phase 1 matches the input document. All the other phases match the document after phase 1, so
they see the new ids and classes. See [the cascade](cascade.md).

## Phase 1: identity

| Property | Effect |
|---|---|
| `-dcs-id: <value>` | sets `attributes['id']` |
| `-dcs-id: auto` | sets the id from the block's text, only where no id exists |
| `-dcs-class: +a -b c` | adds `a` and `c`, and removes `b` |
| `--name: <value>` | sets `attributes['name']` |
| `--name: unset` | removes `attributes['name']` |

`--id`, `--class` and `--heading_level` are refused (diagnostic `reserved-attribute`). Use
`-dcs-id`, `-dcs-class` and `-dcs-as: hN` for them. This rule makes sure that a heading's level
and its rank always agree.

## Phase 2: prune

| Property | Effect |
|---|---|
| `display: none` | drops the block and all the blocks under it |
| `-dcs-prune: section` | drops a heading and its section, up to the next heading of the same or a higher rank |
| `-dcs-dedupe: text` | drops a block whose text repeats the text of an earlier block that the same property matched |

After a prune, dmb numbers `element_order` again from 0.

## Phase 3: structure

| Property | Effect |
|---|---|
| `display: contents` | removes the block, keeps its children, and moves them up one level |
| `-dcs-wrap: blockquote \| details \| callout(x)` | puts the block in a new container |
| `-dcs-as: <element_type>` | changes the block's type |
| `-dcs-as: h1` … `h6` | makes the block a heading and sets `heading_level` |
| `-dcs-level: +n \| -n \| n` | changes a heading's rank. The result stays in 1–6 |

```sql
SELECT u.element_order, u.element_type, u.content, u.level, u.attributes
FROM (SELECT unnest(dcs_apply(doc, '.comment { -dcs-wrap: callout(note); } h2 { -dcs-as: h3; }')) AS u FROM docs);
```

| element_order | element_type | content          | level | attributes             |
|--------------:|--------------|------------------|------:|------------------------|
| 0             | heading      | Field guide      | 1     | `{heading_level=1}`    |
| 1             | paragraph    |                  | 1     | `{}`                   |
| 2             | text         | Ducks are birds. | 2     | `{}`                   |
| 3             | heading      | Comments         | 1     | `{heading_level=3}`    |
| 4             | div          |                  | 1     | `{class=callout note}` |
| 5             | paragraph    |                  | 2     | `{class=comment}`      |
| 6             | text         | Nice post!       | 3     | `{}`                   |

The wrap adds a `div` at row 4. The paragraph and its text move one level down, into the `div`.

## Phase 4: content

| Property | Effect |
|---|---|
| `content` on a block | replaces the block's text |
| `content` on `::before` or `::after` | adds generated content: inline by default |
| `-dcs-content-place: block` | puts the generated content in a new sibling block |
| `-dcs-content-as: <format>` | marks the generated content as markup in that format |
| `text-transform` | changes the case of the block's text, and of the blocks under it |
| `-dcs-replace: "re" "with"` | replaces each match of the regular expression (RE2) |
| `counter-reset`, `counter-increment` | counters, read with `counter(name)` in `content` |

```sql
SELECT u.element_order, u.element_type, u.content
FROM (SELECT unnest(dcs_apply(doc, 'h1 { text-transform: uppercase; } p { -dcs-replace: "birds" "waterfowl"; }')) AS u FROM docs);
```

| element_order | element_type | content              |
|--------------:|--------------|----------------------|
| 0             | heading      | FIELD GUIDE          |
| 1             | paragraph    |                      |
| 2             | text         | Ducks are waterfowl. |
| 3             | heading      | Comments             |
| 4             | paragraph    |                      |
| 5             | text         | Nice post!           |

Generated content with `-dcs-content-as` stays a `raw` row until a reader parses it. The
`dcs_parse_markdown` and `dcs_parse_html` macros do that. See the
[¶ anchor sheet](../cookbook.md#a-anchor-after-every-heading).

### Counters

Put `counter-reset` on a parent or on an earlier sibling. This sheet numbers the `h2` headings
under each `h1`:

```sql
SELECT u.element_order, u.kind, u.element_type, u.content
FROM (SELECT unnest(dcs_apply([
  blk('heading', 'Ducks',    1, MAP {'heading_level': '1'}, 0),
  blk('heading', 'Dabbling', 1, MAP {'heading_level': '2'}, 1),
  blk('heading', 'Diving',   1, MAP {'heading_level': '2'}, 2),
  blk('heading', 'Geese',    1, MAP {'heading_level': '1'}, 3),
  blk('heading', 'Grey',     1, MAP {'heading_level': '2'}, 4)
], 'h1 { counter-reset: s; } h2 { counter-increment: s; } h2::before { content: counter(s) ". "; }')) AS u);
```

| element_order | kind   | element_type | content  |
|--------------:|--------|--------------|----------|
| 0             | block  | heading      | Ducks    |
| 1             | block  | heading      | Dabbling |
| 2             | inline | text         | `1. `    |
| 3             | inline | text         | Dabbling |
| 4             | block  | heading      | Diving   |
| 5             | inline | text         | `2. `    |
| 6             | inline | text         | Diving   |
| 7             | block  | heading      | Geese    |
| 8             | block  | heading      | Grey     |
| 9             | inline | text         | `1. `    |
| 10            | inline | text         | Grey     |

A `::before` moves the heading's text into an inline row, after the generated row.

!!! warning "`:root { counter-reset: … }` restarts at every top-level block"
    `:root` matches every block with no parent. In a document of top-level headings, each
    heading is a `:root`, so it resets the counter before it increments it. With
    `:root { counter-reset: h; } heading { counter-increment: h; }`, every heading shows `1.`
    (measured). Leave out the reset, or put it on a parent block.

## Phase 5: order

`order: <integer>` moves a block among its siblings, with all the blocks under it. The sort is
stable, and the default is 0.

```sql
SELECT u.element_order, u.element_type, u.content
FROM (SELECT unnest(dcs_apply([
  blk('list',      '',        1, MAP {},               0),
  blk('list_item', 'Mallard', 2, MAP {},               1),
  blk('list_item', 'Teal',    2, MAP {'class': 'pin'}, 2),
  blk('list_item', 'Wigeon',  2, MAP {},               3)
], '.pin { order: -1; }')) AS u);
```

| element_order | element_type | content |
|--------------:|--------------|---------|
| 0             | list         |         |
| 1             | list_item    | Teal    |
| 2             | list_item    | Mallard |
| 3             | list_item    | Wigeon  |

## Phase 6: metadata

`@frontmatter { key: value; }` merges keys into the document's front matter block. If the
document has no front matter block, dmb adds one at the start. The values are YAML strings in
double quotes. `text(sel)` is the text of the first block that `sel` matches.

## Phase 7: presentation

Any other CSS property, such as `color`, goes into `attributes['style']`, for a renderer to use.
The winning declarations are written in source order, after any style that the block already has.

```sql
SELECT u.element_order, u.element_type, u.content, u.attributes
FROM (SELECT unnest(dcs_apply(doc, '@frontmatter { title: text(h1); tags: "birds"; }
.comment { color: gray; font-style: italic; }')) AS u FROM docs);
```

| element_order | element_type | content | attributes |
|--------------:|--------------|---------|------------|
| 0 | metadata | (see below) | `{role=frontmatter}` |
| 1 | heading | Field guide | `{heading_level=1}` |
| 2 | paragraph | | `{}` |
| 3 | text | Ducks are birds. | `{}` |
| 4 | heading | Comments | `{heading_level=2}` |
| 5 | paragraph | | `{class=comment, style='color: gray; font-style: italic'}` |
| 6 | text | Nice post! | `{}` |

The `content` of row 0:

```yaml
title: "Field guide"
tags: "birds"
```
