#pragma once

#include "block_tree.hpp"
#include "sheet.hpp"

namespace duckdb {
namespace dmb {

//! One change a rule made to one block, for dcs_explain (the flip view).
struct Change {
	int32_t element_order; // the block's order in the INPUT
	int32_t rule;
	string property;
	Value before; // VARCHAR or NULL
	Value after;  // VARCHAR or NULL
	SourcePos pos; // the winning declaration's position
};

struct ApplyResult {
	vector<Value> blocks;
	vector<Change> changes;
	vector<Diagnostic> diagnostics;
};

//! Apply a sheet to one document (`list`, a duck_block[] Value of type LIST(block_type)).
//! Never throws on sheet problems: they become diagnostics.
//! The diagnostics a sheet has without any document: the parse, and every declaration check that
//! needs no block (values, functions, properties). dcs_check returns these.
vector<Diagnostic> CheckSheet(const Sheet &sheet);

ApplyResult Apply(const Value &list, const BlockFields &fields, const LogicalType &block_type, const Sheet &sheet);

} // namespace dmb
} // namespace duckdb
