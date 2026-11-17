# Limits

## What does not work yet

dmb gives a `not-yet` diagnostic for each of these. It never skips one silently.

- **`:external` and `:internal`.** dmb skips a rule that uses one.
- **`text-overflow` and `-dcs-link`.**
- **`@import`.**
- **`-dcs-as: table`.** dmb marks a `:data` code block with `attributes['dcs-as'] = 'table'`, but
  nothing converts the block to a table yet. The conversion waits for
  [duckdb_panduck#188](https://github.com/teaguesterling/duckdb_panduck/issues/188).
- **`@media (format: X)`.** dmb parses these rules, but `dcs_apply` has no output format yet. So
  these rules never apply.
- **`-dcs-content-as` formats other than markdown and html.** No reader for one row exists yet.
  The content stays a `raw` row with `attributes['format']` (diagnostic `no-reader`).

One more feature is planned but not built, and it does not give `not-yet`. `slug(v, ascii)`, an
ASCII (Punycode) form of `slug()`, gives `bad-value` ("slug() takes one value") from `dcs_apply`
in strict mode. `dcs_check` does not report it (measured 2026-11-16).

## Nesting

Parentheses and brackets can nest 32 levels deep in a sheet or a selector, outside strings and
comments. The selector compiler can nest 256 levels: compounds, suffixes such as `.a.b`, and
pseudo-class arguments. Deeper nesting gives a `syntax` diagnostic. The test suite sends
100,000-deep functions, `:not()`, braces, `@media` and suffix runs. Each gets a diagnostic, not a
crash.

## Performance on large documents

Two possible slowdowns are known. Neither one has been timed, so both are inferred from the code.

- **Sibling combinators.** `h2 + p` and `h2 ~ p` can take time proportional to the square of the
  number of siblings, on a flat document with many top-level blocks.
- **Descendant combinators.** The matcher backtracks with no memo. A long descendant chain on a
  deep document can take much more time.

## How dmb differs from ast_select

sitting_duck's `ast_select` also runs CSS selectors, over syntax trees.
`test/differential/ast_select.sh` compares `dcs_select` with `ast_select_from` on the same
document. On 2026-11-15, all 21 shared selectors agreed. The comparison leaves out these forms,
where the two differ:

| Selector | dmb | ast_select |
|---|---|---|
| `:nth-child(odd)`, `:nth-child(2n+1)` | as CSS | matches nothing ([sitting_duck#337](https://github.com/teaguesterling/sitting_duck/issues/337)) |
| `:not(heading + paragraph)` | as CSS | ignores the argument and matches every paragraph ([sitting_duck#338](https://github.com/teaguesterling/sitting_duck/issues/338)) |
| `:has(> x)` | supported | refuses combinators inside `:has()` |
| `a > b > c > d` | supported | refuses chains of more than two steps |
| `:only-child` | supported | not supported |
| `A, B` | one rule per selector | returns no rows |
| `[attr]` | any attribute | a fixed list of attributes |
| `:contains()`, `:section()`, `:h()`, role pseudo-classes | supported | not supported |

## Loading

A local build loads only in its own CLI (`build/release/duckdb`). See
[getting started](getting-started.md#load).
