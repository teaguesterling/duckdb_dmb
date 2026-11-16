#pragma once

#include "duckdb.hpp"
#include "selector.hpp"

namespace duckdb {
namespace dmb {

//! 1-based line and column. Columns count Unicode code points (plan decision D5).
struct SourcePos {
	int32_t line = 0;
	int32_t column = 0;
};

struct Diagnostic {
	string severity; // "error" | "warning"
	string code;
	string message;
	SourcePos pos;
};

struct ValueToken {
	enum Kind { STRING, IDENT, NUMBER, FUNCTION };
	Kind kind = IDENT;
	string text; // FUNCTION: the function name
	vector<ValueToken> args;
};

struct Declaration {
	string property;
	string value_text; // the source bytes of the value (plan decision D4)
	vector<ValueToken> tokens;
	bool important = false;
	int32_t order = 0; // source order; shared by every selector of a list
	SourcePos pos;
};

struct Rule {
	int32_t index = 0; // after splitting selector lists
	string selector_text;
	ComplexSelector selector;
	string pseudo_element; // "", "before", "after"
	vector<Declaration> declarations;
	string media_format; // from @media (format: X); "" when unscoped
	SourcePos pos;
	bool usable = true; // false: the selector was refused, and Apply skips the rule
};

struct Sheet {
	vector<Rule> rules;
	vector<Declaration> frontmatter; // @frontmatter declarations, in source order (phase 6)
	vector<Diagnostic> diagnostics;
};

//! Parentheses and brackets nest at most this deep in a sheet or selector; deeper is a syntax error.
//! Every recursive pass (tree-sitter walks, the value lexer, the matcher) relies on the bound.
constexpr int MAX_NESTING = 32;
//! Selector compounds and combinators nest at most this deep.
constexpr int MAX_SELECTOR_DEPTH = 256;
bool NestingTooDeep(const string &text);
string NestingMessage();

//! Parse a .dcs sheet. Never throws on bad input: problems become diagnostics.
Sheet ParseSheet(const string &text);

//! Compile one selector from text, as dcs_select and :section() need.
bool CompileSelectorText(const string &text, ComplexSelector &out, string &pseudo_element,
                         vector<Diagnostic> &diagnostics, SourcePos pos);

} // namespace dmb
} // namespace duckdb
