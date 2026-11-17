# Design documents

These are the working documents behind dmb, as the project wrote them. They record decisions and
their reasons, and they use the project's own terms.

- [The DCS design spec](superpowers/specs/2026-11-13-dcs-design.md) (approved 2026-11-13): what
  DCS is, the duck_blocks vocabulary, the properties and the cascade.
- [The v1 implementation plan](superpowers/plans/2026-11-13-dmb-v1.md): the build tasks, and
  decisions D1–D11. These decisions record where dmb differs from the spec, such as the spelling
  of `:section()` and `:matches("…")`.

Where these documents and the other pages on this site disagree, the other pages describe what
dmb does now. Their examples come from runs against a build.

## How dmb works

- **Parsing.** dmb includes tree-sitter and the tree-sitter-css grammar in its source. See
  `third_party/tree-sitter-css/PROVENANCE.md`. dmb needs no other extension when it runs.
- **Matching.** A C++ matcher reads each selector from right to left, over a tree of the blocks in
  memory. It makes no SQL plan for a rule.
