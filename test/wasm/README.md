# WASM checks

`check_wasm_imports.mjs` reads a built `dmb.duckdb_extension.wasm` without running it. It fails
when the module imports a symbol of a dependency (RE2, tree-sitter) that it does not define itself.
Such a symbol is not in the duckdb-wasm host either, so the call fails at run time.

The loadable wasm module is linked by a separate `emcc -sSIDE_MODULE=2` step, which links only the
libraries in `DUCKDB_EXTENSION_DMB_LINKED_LIBS` (see `CMakeLists.txt`). The duckdb-wasm host exports
DuckDB's RE2 wrapper (`duckdb_re2::Regex`), but not the `RE2` class that dmb calls.

```sh
make wasm_eh
node test/wasm/check_wasm_imports.mjs build/wasm_eh/extension/dmb/dmb.duckdb_extension.wasm \
    --forbid '^_Z(NK?|T[VTIS]N)10duckdb_re2' --forbid '^(ts_|tree_sitter_)' --verbose
```

It needs only `node`: no duckdb-wasm engine and no version match.
