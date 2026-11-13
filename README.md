# dmb — Duck Mutation Blocks

A DuckDB extension that runs **DCS** (`.dcs`, Cascading Duck Sheets): CSS-shaped rules that
transform a duck_blocks document. Rules prune, flatten, wrap, retag, add content and anchors,
set attributes and front matter, and carry presentation through to any renderer.

```css
h2, h3 { -dcs-id: auto; }
h2::after, h3::after { content: " [¶](#" attr(id) ")"; -dcs-content-as: markdown; }
:chrome, :meta { display: none; }
```

**Status: being built.** Nothing works yet. The approved design is
[`docs/superpowers/specs/2026-11-13-dcs-design.md`](docs/superpowers/specs/2026-11-13-dcs-design.md).

Builds on [duck_block_utils](https://github.com/teaguesterling/duckdb_duck_block_utils), follows the
[duckent](https://github.com/teaguesterling/duckent) tree contract, and parses sheets with
tree-sitter-css, as [sitting_duck](https://github.com/teaguesterling/sitting_duck)'s `ast_select` does.
