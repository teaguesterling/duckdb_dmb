#pragma once

#include "block_tree.hpp"
#include "sheet.hpp"

namespace duckdb {
namespace dmb {

//! A value token, read from a declaration's source text by dmb's own tokenizer (tree-sitter's
//! value parse is unreliable for DCS: see the Task 3 rulings).
struct Token {
	enum Kind { STRING, IDENT, NUMBER, FUNCTION };
	Kind kind = IDENT;
	string text; // STRING: unescaped; FUNCTION: the name
	vector<vector<Token>> args; // FUNCTION: one token list per comma-separated argument
};

//! Tokenize a value. False (with `error` set) on an unterminated string or unbalanced parens.
bool TokenizeValue(const string &text, vector<Token> &out, string &error);

//! The shared slug rule: pandoc's auto_identifiers (an empty result is "section"), without de-duplication.
string Slug(const string &text);

//! Evaluates values against one block: strings concatenate; attr(x), text(), slug(v), url().
class ValueEvaluator {
public:
	explicit ValueEvaluator(const BlockTree &tree);
	//! False (with `error` naming the problem) on an unknown function or a bad argument.
	//! `is_null` is set when the value is NULL, as url() is with no source.
	bool Eval(const vector<Token> &tokens, idx_t node, string &out, bool &is_null, string &error) const;

private:
	const BlockTree &tree;
};

} // namespace dmb
} // namespace duckdb
