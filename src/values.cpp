#include "values.hpp"

#include "utf8proc.hpp"

namespace duckdb {
namespace dmb {

namespace {

bool IsIdentChar(char c) {
	return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || (static_cast<unsigned char>(c) >= 0x80);
}

struct Lexer {
	const string &s;
	size_t i = 0;
	string error;

	explicit Lexer(const string &s_p) : s(s_p) {
	}

	void SkipSpace() {
		while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
			i++;
		}
	}

	bool String(Token &tok) {
		char q = s[i++];
		tok.kind = Token::STRING;
		while (i < s.size() && s[i] != q) {
			if (s[i] == '\\' && i + 1 < s.size()) {
				char e = s[i + 1];
				// \" \' \\ unescape; any other escape stays as written (a regex wants \d kept)
				if (e == q || e == '\\') {
					tok.text += e;
				} else {
					tok.text += '\\';
					tok.text += e;
				}
				i += 2;
				continue;
			}
			tok.text += s[i++];
		}
		if (i >= s.size()) {
			error = "unterminated string";
			return false;
		}
		i++; // closing quote
		return true;
	}

	//! Tokens until `stop` (')' or ',') or the end; args are split on top-level commas.
	bool List(vector<Token> &out, bool in_args) {
		while (true) {
			SkipSpace();
			if (i >= s.size()) {
				if (in_args) {
					error = "unbalanced parentheses";
					return false;
				}
				return true;
			}
			char c = s[i];
			if (in_args && (c == ')' || c == ',')) {
				return true;
			}
			if (c == ')' || c == ',') {
				if (c == ',') {
					Token t;
					t.kind = Token::IDENT;
					t.text = ",";
					out.push_back(t);
					i++;
					continue;
				}
				error = "unbalanced parentheses";
				return false;
			}
			Token tok;
			if (c == '"' || c == '\'') {
				if (!String(tok)) {
					return false;
				}
			} else {
				size_t start = i;
				while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '(' &&
				       s[i] != ')' && s[i] != ',' && s[i] != '"' && s[i] != '\'') {
					i++;
				}
				tok.text = s.substr(start, i - start);
				bool ident = !tok.text.empty() && IsIdentChar(tok.text[0]) && !std::isdigit(static_cast<unsigned char>(tok.text[0]));
				if (i < s.size() && s[i] == '(' && ident) {
					tok.kind = Token::FUNCTION;
					i++;
					while (true) {
						vector<Token> arg;
						if (!List(arg, true)) {
							return false;
						}
						if (!arg.empty()) {
							tok.args.push_back(std::move(arg));
						}
						if (s[i] == ',') {
							i++;
							continue;
						}
						i++; // ')'
						break;
					}
				} else if (tok.text.empty()) {
					error = "unexpected '" + string(1, s[i]) + "'";
					return false;
				} else {
					char f = tok.text[0];
					tok.kind = (std::isdigit(static_cast<unsigned char>(f)) || f == '+' || f == '-' || f == '.') &&
					                   tok.text.find_first_of("0123456789") != string::npos
					               ? Token::NUMBER
					               : Token::IDENT;
				}
			}
			out.push_back(std::move(tok));
		}
	}
};

} // namespace

bool TokenizeValue(const string &text, vector<Token> &out, string &error) {
	Lexer lex(text);
	if (!lex.List(out, false)) {
		error = lex.error;
		return false;
	}
	return true;
}

string Slug(const string &text) {
	string out;
	auto data = reinterpret_cast<const utf8proc_uint8_t *>(text.data());
	utf8proc_ssize_t len = static_cast<utf8proc_ssize_t>(text.size());
	utf8proc_ssize_t pos = 0;
	while (pos < len) {
		utf8proc_int32_t cp;
		auto n = utf8proc_iterate(data + pos, len - pos, &cp);
		if (n <= 0) {
			pos++;
			continue;
		}
		pos += n;
		if (cp == ' ') {
			out += '-';
			continue;
		}
		if (cp == '-' || cp == '_') {
			out += static_cast<char>(cp);
			continue;
		}
		auto cat = utf8proc_category(cp);
		bool keep = (cat >= UTF8PROC_CATEGORY_LU && cat <= UTF8PROC_CATEGORY_LO) ||
		            (cat >= UTF8PROC_CATEGORY_ND && cat <= UTF8PROC_CATEGORY_NO) ||
		            (cat >= UTF8PROC_CATEGORY_MN && cat <= UTF8PROC_CATEGORY_ME);
		if (!keep) {
			continue;
		}
		utf8proc_uint8_t buf[4];
		auto w = utf8proc_encode_char(utf8proc_tolower(cp), buf);
		out.append(reinterpret_cast<const char *>(buf), static_cast<size_t>(w));
	}
	return out;
}

ValueEvaluator::ValueEvaluator(const BlockTree &tree_p) : tree(tree_p) {
}

bool ValueEvaluator::Eval(const vector<Token> &tokens, idx_t node, string &out, bool &is_null, string &error) const {
	out.clear();
	is_null = false;
	bool prev_word = false;
	for (auto &tok : tokens) {
		string piece;
		bool word = false;
		switch (tok.kind) {
		case Token::STRING:
			piece = tok.text;
			break;
		case Token::IDENT:
		case Token::NUMBER:
			piece = tok.text;
			word = true;
			break;
		case Token::FUNCTION: {
			auto &name = tok.text;
			if (name == "text") {
				if (!tok.args.empty()) {
					error = "text() takes no argument here";
					return false;
				}
				piece = tree.Text(node);
			} else if (name == "attr") {
				if (tok.args.size() != 1 || tok.args[0].size() != 1 || tok.args[0][0].kind == Token::FUNCTION) {
					error = "attr() takes one attribute name";
					return false;
				}
				auto key = tok.args[0][0].text;
				auto v = tree.Attr(node, key);
				piece = v ? *v : string();
			} else if (name == "slug") {
				if (tok.args.size() != 1) {
					error = "slug() takes one value";
					return false;
				}
				string inner;
				bool inner_null;
				if (!Eval(tok.args[0], node, inner, inner_null, error)) {
					return false;
				}
				piece = Slug(inner);
			} else if (name == "url") {
				// the document's own source: attributes['source'] on a level-0 document root (inferred)
				const string *src = nullptr;
				for (idx_t i = 0; i < tree.nodes.size() && !src; i++) {
					if (tree.nodes[i].element_type == "document") {
						src = tree.Attr(i, "source");
					}
				}
				if (!src) {
					is_null = true;
					return true;
				}
				piece = *src;
			} else {
				error = "unknown-function: " + name + "()";
				return false;
			}
			break;
		}
		}
		if (word && prev_word) {
			out += ' ';
		}
		out += piece;
		prev_word = word;
	}
	return true;
}

} // namespace dmb
} // namespace duckdb
