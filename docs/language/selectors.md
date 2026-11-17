# Selectors

A DCS selector is a CSS selector over the blocks of a document. dmb never matches inline rows.

## The block tree

dmb reads a document as a tree. A block's parent is the nearest earlier row with a smaller
`level`. The children of a block are its siblings in that tree. Combinators and structural
pseudo-classes use this tree.

## Simple selectors

| Selector | Matches |
|---|---|
| `paragraph`, `heading`, `list`, … | a block whose `element_type` is that name |
| `h1` … `h6` | a heading whose `attributes['heading_level']` is that rank |
| `p`, `img`, `li`, `pre` | names for `paragraph`, `image`, `list_item` and `code` |
| `#id` | `attributes['id']` |
| `.class` | one word of `attributes['class']` |
| `[attr]`, `[attr=v]` | an attribute; also `~=`, `^=`, `$=`, `*=` and `\|=` |
| `[kind]`, `[element_type]`, `[encoding]`, `[level]` | the block's own fields |
| `*` | every block |

## Combinators

The four CSS combinators work: descendant (a space), child (`>`), next sibling (`+`) and later
sibling (`~`).

```sql
SELECT dcs_select(doc, 'heading') AS headings, dcs_select(doc, 'p') AS paragraphs FROM docs;
```

| headings | paragraphs |
|----------|------------|
| [0, 3]   | [1, 4]     |

`dcs_select` returns the `element_order` of each block that the selector matches.

## Pseudo-classes

### CSS structural

`:first-child`, `:last-child`, `:only-child`, `:nth-child()`, `:nth-last-child()`,
`:first-of-type`, `:last-of-type`, `:only-of-type`, `:nth-of-type()`, `:nth-last-of-type()`,
`:empty` and `:root`.

`:root` matches every block with no parent. A document can have many top-level blocks, so
`:root` can match many blocks.

`:not()` and `:has()` take a selector. `:has()` also takes a relative selector, such as
`:has(> list_item)`.

### Text

| Pseudo-class | Matches |
|---|---|
| `:contains("…")` | a block whose text contains the string |
| `:matches("regex")` | a block whose text matches the regular expression (RE2 syntax) |

A block's text is its `content` plus the text of its inline rows.

### Headings and sections

| Pseudo-class | Matches |
|---|---|
| `:heading` | any heading |
| `:h(n)` | a heading of rank `n`, from 1 to 6 |
| `:section(sel)` | a heading that matches `sel`, and every block after it up to the next heading of the same or a higher rank |

```sql
SELECT dcs_select(doc, ':section(h2:contains("Comments"))') AS section_rows FROM docs;
```

| section_rows |
|--------------|
| [3, 4]       |

### Document roles

These pseudo-classes come from the duck_blocks vocabulary. They read the `role` attribute that
readers record, and the shape of a block.

| Pseudo-class | Matches |
|---|---|
| `:chrome` | `role` is `nav`, `header`, `footer` or `aside`; or a paragraph that holds only a link |
| `:main` | `role=main`; else the one `role=article` block, when no block has `role=main` |
| `:meta` | a `metadata` block (such as front matter), or a row of kind `value` |
| `:body` | a block that is not `:meta` and is not inside a `:meta` block |
| `:data` | a `table`, or a `code` block in csv, tsv, json, jsonl or yaml |
| `:callout`, `:footnote`, `:caption` | by type and class |
| `:link-only`, `:image-only` | a paragraph that holds only a link, or only an image |
| `:lang(x)` | `attributes['lang']` |

The [cookbook](../cookbook.md#reader-mode) uses `:chrome` and `:main` for reader mode.

## Pseudo-elements

`::before` and `::after` add generated content before or after a block's text. They take the
`content` property. See [properties](properties.md#phase-4-content). A pseudo-element must be the
last part of a selector.

## Selector lists

`h1, h2 { … }` is two rules, one for each selector. Each rule has its own specificity.

## Nesting limit

Parentheses and brackets can nest 32 levels deep in a sheet or a selector, outside strings and
comments. The selector compiler can nest 256 levels. Deeper nesting gives a `syntax` diagnostic.
