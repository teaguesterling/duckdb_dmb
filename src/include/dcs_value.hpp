#pragma once
// The DCS type: a parsed sheet as a DuckDB value, for dmb's functions to hand to each other
// (spec 2026-11-18-dcs-type-design.md).

#include "duckdb.hpp"
#include "sheet.hpp"

namespace duckdb {
class ExtensionLoader;

namespace dmb {

//! LIST(STRUCT(selector, meta, declarations LIST(STRUCT(property, value, extra JSON)), extra JSON)).
//! Registered without an alias: an aliased list type does not match the list functions on DuckDB v2.0.1.
LogicalType DcsType();

//! A sheet as a DCS value: one entry per rule, then one @frontmatter entry if there is one.
//! The sheet's own diagnostics (the text-level ones) are not kept.
Value SheetToDcs(const Sheet &sheet);

//! A JSON object of the given keys, in order; a key with a NULL value is left out. "" when all are.
string WriteExtra(const vector<std::pair<string, Value>> &keys);

void RegisterDcsType(ExtensionLoader &loader);

} // namespace dmb
} // namespace duckdb
