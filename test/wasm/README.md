# WASM checks

Two checks, both run by the `WASM checks` job in `.github/workflows/MainDistributionPipeline.yml`.

## 1. Static import check

`check_wasm_imports.mjs` reads a built `dmb.duckdb_extension.wasm` without running it. It fails
when the module imports a symbol of a dependency (RE2, tree-sitter) that it does not define itself.
Such a symbol is not in the duckdb-wasm host either, so the call fails at run time.

The loadable wasm module is linked by a separate `emcc -sSIDE_MODULE=2` step, which links only the
libraries in `DUCKDB_EXTENSION_DMB_LINKED_LIBS` (see `CMakeLists.txt`). The duckdb-wasm host exports
DuckDB's RE2 wrapper (`duckdb_re2::Regex`), but not the `RE2` class that dmb calls.

`run_wasm_checks.sh` runs it, with dmb's forbid set, over every module under the directories it is
given (local build dirs by default, or downloaded CI artifacts):

```sh
make wasm_eh
test/wasm/run_wasm_checks.sh            # build/wasm_mvp, build/wasm_eh, build/wasm_threads if present
```

It needs only `node`: no duckdb-wasm engine and no version match.

## 2. Live load test

`load_test.cjs` (adapted from duckdb_webbed) serves a local extension repository, starts duckdb-wasm
in node, runs `INSTALL dmb` and `LOAD dmb`, and runs one query. The CI query compiles a regular
expression, so a module without RE2 fails here:

```sh
cd test/wasm && npm ci --ignore-scripts
mkdir -p repo/v2.0.1/wasm_eh && cp ../../build/wasm_eh/extension/dmb/dmb.duckdb_extension.wasm repo/v2.0.1/wasm_eh/
node load_test.cjs --repo repo --name dmb --platform eh \
  --query "SELECT (SELECT code FROM dcs_check('p { -dcs-replace: \"(\" \"x\"; }')) AS code" \
  --expect '"code":"bad-regex"'
```

The engine must be built from the DuckDB commit dmb pins. `package.json` pins
`@duckdb/duckdb-wasm` 1.33.1-dev124.0 (a dev prerelease), which reports v2.0.1, source_id
`8d78114e90`, platform `wasm_eh`. Only wasm_eh is tested live: the package has no mvp bundle, and its
threads bundle has no node worker.
