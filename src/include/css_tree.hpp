#pragma once
// Internal: tree-sitter plumbing shared by the sheet parser and the selector compiler.

#include "sheet.hpp"
#include "tree_sitter/api.h"

extern "C" const TSLanguage *tree_sitter_css(void);

namespace duckdb {
namespace dmb {

//! Maps a byte offset in the whole sheet to a 1-based line and code-point column.
class LineIndex {
public:
	explicit LineIndex(const string &text);
	SourcePos Pos(uint32_t byte) const;

private:
	const string &text;
	vector<uint32_t> starts;
};

//! One parse of one piece of text, freed on destruction.
class TsTree {
public:
	explicit TsTree(const string &text);
	~TsTree();
	TsTree(const TsTree &) = delete;
	TsTree &operator=(const TsTree &) = delete;
	TSNode Root() const;

private:
	TSParser *parser;
	TSTree *tree;
};

//! The text a tree was parsed from, and how its offsets map back to the sheet.
struct TsCtx {
	const string &src;
	uint32_t base;           // offset of src within the sheet
	const LineIndex *lines;  // null: every position is `fixed`
	SourcePos fixed;
	vector<Diagnostic> &diagnostics;

	string Text(TSNode n) const;
	SourcePos Pos(TSNode n) const;
	void Diag(const string &severity, const string &code, const string &message, TSNode n);
};

string NodeType(TSNode n);

//! Compile one selector node (a named child of `selectors`). False: the rule can't be used.
bool CompileSelectorNode(TsCtx &ctx, TSNode node, ComplexSelector &out, string &pseudo_element);

} // namespace dmb
} // namespace duckdb
