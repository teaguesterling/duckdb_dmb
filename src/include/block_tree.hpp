#pragma once

#include "duckdb.hpp"

#include <map>

namespace duckdb {
namespace dmb {

//! Where each duck_block field sits in the caller's struct (bind resolves it by name).
struct BlockFields {
	idx_t kind, element_type, content, level, encoding, attributes, element_order;
};

//! Validate that `type` is a list of duck_block-shaped structs. Throws BinderException naming
//! the first missing field.
BlockFields ResolveBlockFields(const LogicalType &type, const string &function_name);

struct BlockNode {
	idx_t row = 0;
	int64_t parent = -1;   // nearest preceding row with a smaller level
	vector<idx_t> children; // matchable children only
	idx_t subtree_end = 0; // one past the last descendant
	string kind, element_type, content, encoding;
	int32_t level = 0;
	int32_t element_order = 0;
	std::map<string, string> attributes;
};

class BlockTree {
public:
	static BlockTree FromList(const Value &list, const BlockFields &fields);

	vector<BlockNode> nodes;
	vector<idx_t> roots; // matchable nodes with no matchable parent

	bool Matchable(idx_t i) const;
	//! A heading's flattened content when it has one; else its inline descendants' text in order.
	string Text(idx_t i) const;
	vector<string> Classes(idx_t i) const;
	const string *Attr(idx_t i, const string &key) const;
	//! The matchable siblings of i, including i, in order.
	const vector<idx_t> &Siblings(idx_t i) const;
	int32_t HeadingRank(idx_t i) const; // 0 when not a heading or no heading_level
};

} // namespace dmb
} // namespace duckdb
