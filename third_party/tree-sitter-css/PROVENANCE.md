# tree-sitter-css, vendored

| | |
|---|---|
| Grammar | tree-sitter/tree-sitter-css v0.25.0, commit `dda5cfc5722c429eaba1c910ca32c2c0c5bb1a3f` |
| Copied from | sitting_duck `generated_parsers/tree-sitter-css/src/` (sitting_duck `v1.18.3-2-g821af42`), 2026-11-13 |
| ABI | `LANGUAGE_VERSION 15` (`parser.c:17`) |
| Runtime | tree-sitter `eaa10b279f208b47f65e77833d65763f072f3030`, the `third_party/tree-sitter` submodule; the same commit sitting_duck pins |
| License | MIT (`LICENSE`, from the grammar repo) |
| sha256 `parser.c` | `193cd8fcfc98d88052208842343280cde8e369bc92df0c3b084e381056eddca5` |
| sha256 `scanner.c` | `18dae8c8c4f515f28a3dc7ffb5bda259b06013a752921dc411a2fad8ecf78988` |

**Why these sources, not a fresh `tree-sitter generate`:** they are byte-identical to what
sitting_duck compiles, so a selector that parses one way in `ast_select` parses the same way
here. Keep the runtime pin equal to sitting_duck's: a static link of both extensions into
duckeye's Android engine will need one copy of the `ts_*` symbols (inferred, not yet measured).

**To bump:** copy the new generated `src/` from sitting_duck, update this table, and check
that `dcs_parser_abi()` in `test/sql/dmb.test` still matches.
