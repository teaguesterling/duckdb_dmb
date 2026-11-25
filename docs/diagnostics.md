# Diagnostics

A diagnostic is a problem that dmb found in a sheet. A diagnostic never stops a document. dmb
parses a sheet one top-level rule at a time, so one bad rule does not remove the rules after it.
dmb skips a rule or a declaration that it cannot use, and gives a diagnostic.

## Where diagnostics show

- `dcs_check(sheet)` lists the diagnostics of a sheet as rows. It needs no document.
- `dcs_apply(blocks, sheet, true)` raises the first diagnostic as an error. This is strict mode.
- Some diagnostics need a document, such as `-dcs-level` on a block that is not a heading. These
  come only from `dcs_apply`, in strict mode.

Lines start at 1. Columns start at 1 and count characters, not bytes.

```sql
SELECT * FROM dcs_check('p:external { display: none; }
@import "x.dcs";
@font-face { font-family: x; }
h1 { -dcs-colour: red; }
li:nth-child(odd { a: b; }');
```

| severity | code             | message                                                           | line | column |
|----------|------------------|-------------------------------------------------------------------|-----:|-------:|
| warning  | not-yet          | not-yet: dmb does not match :external yet                         | 1    | 1      |
| warning  | not-yet          | @import is not supported yet                                      | 2    | 1      |
| warning  | not-yet          | this at-rule is not supported yet: @font-face { font-family: x; } | 3    | 1      |
| error    | syntax           | cannot parse selector li:nth-child(odd                            | 5    | 1      |
| error    | unknown-property | unknown property -dcs-colour                                      | 4    | 6      |

The rows are not always in line order.

## Codes

| Code | Severity | Meaning |
|---|---|---|
| `syntax` | error | dmb cannot parse the text, or the nesting is too deep |
| `unknown-property` | error | a `-dcs-` property that dmb does not know |
| `unknown-pseudo-class` | error | a pseudo-class that dmb does not know |
| `unknown-pseudo-element` | error | a pseudo-element other than `::before` and `::after` |
| `unknown-function` | error | a value function that dmb does not know |
| `unsupported-selector` | error | the CSS nesting selector `&` |
| `selector-list` | error | a selector list where one selector is necessary, such as in `text()` |
| `bad-value` | error | a value that the property cannot use |
| `empty-value` | error | a declaration with no value |
| `bad-argument` | error | a pseudo-class argument that is not correct, such as `:h(7)` |
| `missing-argument` | error | an empty argument, such as `:not()` |
| `bad-nth` | error | an `:nth-child()` argument that is not correct or is too large |
| `bad-regex` | error | a regular expression that RE2 cannot compile |
| `reserved-attribute` | error | `--id`, `--class` or `--heading_level` |
| `pseudo-element-not-last` | error | a pseudo-element that is not at the end of the selector |
| `pseudo-element-in-argument` | error | a pseudo-element inside `:not()`, `:has()` or `:section()` |
| `level-not-heading` | error | `-dcs-level` on a block that is not a heading (needs a document) |
| `prune-section-not-heading` | error | `-dcs-prune: section` on a block that is not a heading (needs a document) |
| `content-on-container` | error | `content` on a block that holds other blocks |
| `content-needs-text-block` | error | inline `::before` or `::after` content on a block that cannot hold text. Use `-dcs-content-place: block` |
| `contents-has-inlines` | error | `display: contents` on a block that holds inline text |
| `as-table-needs-data` | error | `-dcs-as: table` on a block that is not a code block in csv, tsv, json, jsonl or yaml |
| `not-yet` | warning | a feature of DCS that dmb does not support yet. See [limits](limits.md) |
| `unsupported-at-rule` | warning | an at-rule that is not part of DCS, such as `@keyframes` or `@supports` |
| `unsupported-media` | warning | a media query other than `@media (format: X)` |
| `nested-media` | warning | `@media` inside `@media` |
| `nested-rule` | warning | a rule inside a rule |
| `pseudo-only` | warning | a property that applies only to `::before` and `::after` content, used on a block |
| `no-reader` | warning | `-dcs-content-as` with a format that has no reader |
| `bad-extra` | error | a hand-built [`DCS`](reference.md#the-dcs-type) value whose `extra` is not a JSON object, or has a known key of the wrong JSON type |

A hand-built [`DCS`](reference.md#the-dcs-type) value gets the codes that text gets. dmb also
refuses what no text can express, so that every value it accepts can be written as text and read
back:

| Hand-built case | Code |
|---|---|
| a selector with a top-level comma | `selector-list` |
| a pseudo-element in `selector` and also in `extra.pseudo_element` | `pseudo-element-not-last` |
| a `value` that does not tokenize, holds a top-level `;`, `{` or `}`, or ends in `!important` | `bad-value` |
| a `property` that is NULL, empty or not a name; a plain rule with a NULL `selector` | `syntax` |
| a `meta` other than NULL and `'frontmatter'`, two frontmatter entries, a frontmatter entry with `media`, or a `media` that is not a name | `syntax` |
| `extra` that is not a JSON object, or a known key of the wrong JSON type | `bad-extra` |

The position comes from `extra.line` and `extra.column`, and is line 0, column 0 when they are
not there. With the json extension loaded, a cast to `DCS` refuses JSON that does not parse before
dmb sees it.

`@font-face` and other at-rules that tree-sitter-css parses as a general at-rule give `not-yet`.
`@keyframes`, `@supports`, `@charset` and `@namespace` give `unsupported-at-rule` (measured).
