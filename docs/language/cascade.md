# Values and the cascade

## Values

A value is a list of strings and functions. dmb joins the parts with no space between them.

| Function | Gives |
|---|---|
| `"…"` | the string |
| `text()` | the block's text; in `@frontmatter`, `text(sel)` is the text of the first block that `sel` matches |
| `attr(name)` | `attributes['name']` |
| `slug(value)` | an id made from the value (see [ids and slugs](#ids-and-slugs)) |
| `counter(name)` | the counter's current value |
| `url()` | `attributes['source']` of the document's `document` block; NULL if the document has none |

An unknown function gives the diagnostic `unknown-function`.

## The cascade

For each block and each property, one declaration wins. dmb uses the CSS order:

1. A declaration with `!important` wins over one without.
2. Then the higher [specificity](https://developer.mozilla.org/en-US/docs/Web/CSS/Specificity) wins.
3. Then the declaration that comes later in the sheet wins.

```sql
SELECT u.element_order, u.attributes['tone'] AS tone
FROM (SELECT unnest(dcs_apply(doc, 'p { --tone: "plain"; }
.comment { --tone: "warm"; }
p { --tone: "late"; }')) AS u FROM docs)
WHERE u.element_type = 'paragraph';
```

| element_order | tone |
|--------------:|------|
| 1             | late |
| 4             | warm |

Row 4 has the class `comment`, so `.comment` wins on specificity. Row 1 matches only the two `p`
rules, so the later one wins. With `!important` on the first rule, it wins on both rows:

```sql
SELECT u.element_order, u.attributes['tone'] AS tone
FROM (SELECT unnest(dcs_apply(doc, 'p { --tone: "plain" !important; }
.comment { --tone: "warm"; }')) AS u FROM docs)
WHERE u.element_type = 'paragraph';
```

| element_order | tone  |
|--------------:|-------|
| 1             | plain |
| 4             | plain |

## Matching and phases

- dmb matches phase 1 (`-dcs-id`, `-dcs-class`) against the input document.
- dmb matches all the other phases against the document after phase 1. So a class or id that
  phase 1 adds can select blocks in later phases.
- Rules do not see the rows that other rules add, such as generated content.

Here, phase 1 adds the class `gone` to the `h2`, and phase 2 drops the blocks with that class:

```sql
SELECT u.element_order, u.element_type, u.attributes
FROM (SELECT unnest(dcs_apply(doc, 'h2 { -dcs-class: +gone; } .gone { display: none; }')) AS u FROM docs);
```

| element_order | element_type | attributes          |
|--------------:|--------------|---------------------|
| 0             | heading      | `{heading_level=1}` |
| 1             | paragraph    | `{}`                |
| 2             | text         | `{}`                |
| 3             | paragraph    | `{class=comment}`   |
| 4             | text         | `{}`                |

`dcs_explain` shows which rule won each change. See the
[function reference](../reference.md#dcs_explain).

## @media

`@media (format: X) { … }` scopes rules to one output format. dmb parses these rules, and
`dcs_rules` shows the format in its `media` column. But `dcs_apply` has no output format yet, so
these rules never apply. Any other media query gives `unsupported-media`.

## Ids and slugs

`slug()` and `-dcs-id: auto` use pandoc's `auto_identifiers` rule:

- Make the text lower-case across all of Unicode, and keep letters of any script.
- Keep letters, numbers, `_`, `-` and `.`. Drop other characters.
- Join the words with `-`.
- Drop everything before the first letter.
- If nothing is left, the id is `section`.

```sql
SELECT u.attributes['id'] AS id, u.content
FROM (SELECT unnest(dcs_apply([
  blk('heading', 'ÉCOLE Ünd',       1, MAP {'heading_level': '2'}, 0),
  blk('heading', 'Zoë''s notes',    1, MAP {'heading_level': '2'}, 1),
  blk('heading', '1. Introduction', 1, MAP {'heading_level': '2'}, 2),
  blk('heading', '!!!',             1, MAP {'heading_level': '2'}, 3)
], 'heading { -dcs-id: slug(text()); }')) AS u);
```

| id           | content         |
|--------------|-----------------|
| école-ünd    | ÉCOLE Ünd       |
| zoës-notes   | Zoë's notes     |
| introduction | 1. Introduction |
| section      | !!!             |

`-dcs-id: auto` also makes the ids in a document unique: a second `notes` becomes `notes-1`, as
in pandoc. `test/differential/slug_pandoc.sh` compares `slug()` with pandoc 3.1.3. On 2026-11-14
and 2026-11-15, all 33 cases agreed.
