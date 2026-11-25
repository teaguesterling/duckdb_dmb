#pragma once
// The DCS type: a parsed sheet as a DuckDB value, for dmb's functions to hand to each other
// (spec 2026-11-18-dcs-type-design.md).

#include "duckdb.hpp"
#include "sheet.hpp"
#include "duckdb/function/scalar_function.hpp"

#include <map>

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

//! Parse a JSON object of scalars into `out` (strings as VARCHAR, integers as BIGINT, true/false as BOOLEAN,
//! null as a NULL VARCHAR). A nested value is skipped, so unknown keys may hold anything. False, with
//! `error` set, when the text is not one JSON object. A NULL `extra` gives an empty map.
bool ReadExtra(const Value &extra, std::map<string, Value> &out, string &error);

//! A DCS value as dmb's internal sheet. Never throws on bad content: what text could not express becomes a
//! diagnostic, and every other check is left to CheckSheet and Apply, as for text.
Sheet DcsToSheet(const Value &dcs);

//! The sheet a function was given: a DCS value (any LIST) is converted, anything else is parsed as text.
Sheet SheetArgument(const Value &arg);

//! Bind-time: argument `index` becomes DcsType() for a LIST, and VARCHAR for any other non-NULL type.
void ResolveSheetArgument(ResolveScalarFunctionTypesInput &input, idx_t index);

//! A DCS value as .dcs text: one rule per line, in list order, values as stored. Rules with a media format
//! go in @media (format: X) { ... }, one block for each run of adjacent rules with the same format.
string FormatDcs(const Value &dcs);

void RegisterDcsType(ExtensionLoader &loader);

} // namespace dmb
} // namespace duckdb
