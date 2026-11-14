#include "css_tree.hpp"

namespace duckdb {
namespace dmb {

namespace {

//! Split a sheet into top-level statements: a `{...}` block at depth 0, or a `...;` statement.
//! Measured (2026-11-13): parsed whole, one broken rule turns every later rule into ERROR
//! fragments. Parsed one statement at a time, the damage stays in that statement, which is
//! what "errors never abort a document" asks for.
vector<std::pair<uint32_t, uint32_t>> SplitTopLevel(const string &text) {
	vector<std::pair<uint32_t, uint32_t>> chunks;
	uint32_t n = static_cast<uint32_t>(text.size());
	uint32_t start = 0;
	int depth = 0;
	for (uint32_t i = 0; i < n; i++) {
		char c = text[i];
		if (c == '"' || c == '\'') {
			for (i++; i < n && text[i] != c; i++) {
				if (text[i] == '\\') {
					i++;
				} else if (text[i] == '\n') {
					break; // an unterminated string ends at the line, as in CSS
				}
			}
		} else if (c == '/' && i + 1 < n && text[i + 1] == '*') {
			auto end = text.find("*/", i + 2);
			i = end == string::npos ? n : static_cast<uint32_t>(end + 1);
		} else if (c == '{') {
			depth++;
		} else if (c == '}') {
			if (depth > 0) {
				depth--;
			}
			if (depth == 0) {
				chunks.emplace_back(start, i + 1);
				start = i + 1;
			}
		} else if (c == ';' && depth == 0) {
			chunks.emplace_back(start, i + 1);
			start = i + 1;
		}
	}
	if (start < n) {
		chunks.emplace_back(start, n);
	}
	return chunks;
}

bool Blank(const string &s) {
	return s.find_first_not_of(" \t\r\n") == string::npos;
}

ValueToken ToToken(TsCtx &ctx, TSNode n) {
	ValueToken tok;
	auto type = NodeType(n);
	if (type == "string_value") {
		tok.kind = ValueToken::STRING;
		for (uint32_t i = 0; i < ts_node_named_child_count(n); i++) {
			tok.text += ctx.Text(ts_node_named_child(n, i));
		}
	} else if (type == "integer_value" || type == "float_value") {
		tok.kind = ValueToken::NUMBER;
		tok.text = ctx.Text(n);
	} else if (type == "call_expression") {
		tok.kind = ValueToken::FUNCTION;
		for (uint32_t i = 0; i < ts_node_named_child_count(n); i++) {
			auto c = ts_node_named_child(n, i);
			auto ct = NodeType(c);
			if (ct == "function_name") {
				tok.text = ctx.Text(c);
			} else if (ct == "arguments") {
				for (uint32_t j = 0; j < ts_node_named_child_count(c); j++) {
					auto a = ts_node_named_child(c, j);
					if (NodeType(a) != "comment") {
						tok.args.push_back(ToToken(ctx, a));
					}
				}
			}
		}
	} else {
		tok.kind = ValueToken::IDENT;
		tok.text = ctx.Text(n);
	}
	return tok;
}

struct Builder {
	const string &text;
	LineIndex lines;
	Sheet sheet;
	int32_t rule_index = 0;
	int32_t decl_order = 0;

	explicit Builder(const string &text_p) : text(text_p), lines(text_p) {
	}

	bool ParseDeclaration(TsCtx &ctx, TSNode decl, Declaration &out) {
		// The value is the source text between ':' and ';' or '!important' (plan D4). tree-sitter's
		// parse of the value is only a token hint: `-dcs-class: +note -draft` parses as arithmetic
		// with a MISSING operand, and that must not drop the declaration.
		uint32_t vstart = 0, vend = ts_node_end_byte(decl);
		bool have_colon = false;
		for (uint32_t i = 0; i < ts_node_child_count(decl); i++) {
			auto c = ts_node_child(decl, i);
			auto t = NodeType(c);
			if (t == "property_name") {
				out.property = ctx.Text(c);
			} else if (!have_colon && t == ":") {
				vstart = ts_node_end_byte(c);
				have_colon = true;
			} else if (t == "important" || (t == ";" && !ts_node_is_missing(c))) {
				out.important = out.important || t == "important";
				vend = MinValue<uint32_t>(vend, ts_node_start_byte(c));
			} else if (have_colon && ts_node_is_named(c) && !ts_node_is_missing(c) && t != "comment" &&
			           t != "js_comment") {
				out.tokens.push_back(ToToken(ctx, c));
			}
		}
		if (out.property.empty() || !have_colon) {
			ctx.Diag("error", "syntax", "cannot parse declaration " + ctx.Text(decl), decl);
			return false;
		}
		out.value_text = Trim(ctx.src.substr(vstart, vend > vstart ? vend - vstart : 0));
		if (out.value_text.empty()) {
			ctx.Diag("error", "empty-value", "declaration has no value: " + ctx.Text(decl), decl);
			return false;
		}
		while (!out.value_text.empty() && (out.value_text.back() == ' ' || out.value_text.back() == '\t' ||
		                                   out.value_text.back() == '\n' || out.value_text.back() == '\r')) {
			out.value_text.pop_back();
		}
		out.pos = ctx.Pos(decl);
		out.order = decl_order++;
		return true;
	}

	//! Recover `property: value;` pairs from an ERROR node inside a block, by its source text.
	//! Measured: tree-sitter-css makes an ERROR of a declaration whose string holds "{", and of
	//! every declaration after it in the block. Splits on ':' and ';' outside strings (plan D4).
	void Recover(TsCtx &ctx, TSNode err, vector<Declaration> &decls) {
		auto text = ctx.Text(err);
		uint32_t base = ts_node_start_byte(err);
		size_t start = 0;
		for (size_t i = 0; i <= text.size(); i++) {
			char c = i < text.size() ? text[i] : ';';
			if (c == '"' || c == '\'') {
				for (i++; i < text.size() && text[i] != c; i++) {
					if (text[i] == '\\') {
						i++;
					}
				}
				continue;
			}
			if (c != ';') {
				continue;
			}
			auto piece = text.substr(start, i - start);
			auto piece_start = start;
			start = i + 1;
			auto lead = piece.find_first_not_of(" \t\r\n");
			if (lead == string::npos) {
				continue;
			}
			auto colon = piece.find(':');
			string property = colon == string::npos ? string() : piece.substr(lead, colon - lead);
			while (!property.empty() && std::isspace(static_cast<unsigned char>(property.back()))) {
				property.pop_back();
			}
			bool ident = !property.empty();
			for (char pc : property) {
				if (!(std::isalnum(static_cast<unsigned char>(pc)) || pc == '-' || pc == '_')) {
					ident = false;
				}
			}
			SourcePos pos = ctx.lines ? ctx.lines->Pos(ctx.base + base + static_cast<uint32_t>(piece_start + lead))
			                          : ctx.fixed;
			if (!ident) {
				ctx.diagnostics.push_back(
				    Diagnostic {"error", "syntax", "cannot parse declaration " + Trim40(Trim(piece)), pos});
				continue;
			}
			Declaration d;
			d.property = property;
			string value = piece.substr(colon + 1);
			auto bang = value.rfind("!important");
			if (bang != string::npos && Trim(value.substr(bang + 10)).empty()) {
				d.important = true;
				value = value.substr(0, bang);
			}
			value = Trim(value);
			while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
				value.pop_back();
			}
			if (value.empty()) {
				ctx.diagnostics.push_back(
				    Diagnostic {"error", "empty-value", "declaration has no value: " + Trim40(Trim(piece)), pos});
				continue;
			}
			d.value_text = value;
			d.pos = pos;
			d.order = decl_order++;
			decls.push_back(std::move(d));
		}
	}

	void RuleSet(TsCtx &ctx, TSNode rs, const string &media) {
		TSNode selectors, block;
		bool have_sel = false, have_block = false;
		for (uint32_t i = 0; i < ts_node_named_child_count(rs); i++) {
			auto c = ts_node_named_child(rs, i);
			auto t = NodeType(c);
			if (t == "selectors") {
				selectors = c;
				have_sel = true;
			} else if (t == "block") {
				block = c;
				have_block = true;
			}
		}
		if (!have_sel || !have_block) {
			ctx.Diag("error", "syntax", "cannot parse rule " + ctx.Text(rs), rs);
			return;
		}
		vector<Declaration> decls;
		for (uint32_t i = 0; i < ts_node_named_child_count(block); i++) {
			auto c = ts_node_named_child(block, i);
			auto t = NodeType(c);
			if (t == "declaration") {
				Declaration d;
				if (ParseDeclaration(ctx, c, d)) {
					decls.push_back(std::move(d));
				}
			} else if (t == "comment" || t == "js_comment") {
				continue;
			} else if (t == "rule_set") {
				ctx.Diag("warning", "nested-rule", "nested rules are not supported: " + ctx.Text(c), c);
			} else if (t == "ERROR") {
				Recover(ctx, c, decls);
			} else {
				ctx.Diag("error", "syntax", "cannot parse " + ctx.Text(c), c);
			}
		}
		for (uint32_t i = 0; i < ts_node_named_child_count(selectors); i++) {
			auto sel = ts_node_named_child(selectors, i);
			auto st = NodeType(sel);
			if (st == "comment" || st == "js_comment") {
				continue;
			}
			Rule rule;
			rule.index = rule_index++;
			rule.media_format = media;
			rule.pos = ctx.Pos(sel);
			rule.declarations = decls;
			rule.usable = CompileSelectorNode(ctx, sel, rule.selector, rule.pseudo_element);
			rule.selector_text = ctx.Text(sel);
			if (!rule.pseudo_element.empty()) {
				auto cut = rule.selector_text.rfind("::");
				if (cut != string::npos) {
					rule.selector_text = rule.selector_text.substr(0, cut);
				}
			}
			sheet.rules.push_back(std::move(rule));
		}
	}

	void Media(TsCtx &ctx, TSNode ms) {
		string format;
		TSNode block;
		bool have_block = false;
		for (uint32_t i = 0; i < ts_node_named_child_count(ms); i++) {
			auto c = ts_node_named_child(ms, i);
			auto t = NodeType(c);
			if (t == "block") {
				block = c;
				have_block = true;
			} else if (t == "feature_query") {
				string feature, value;
				for (uint32_t j = 0; j < ts_node_named_child_count(c); j++) {
					auto f = ts_node_named_child(c, j);
					if (NodeType(f) == "feature_name") {
						feature = ctx.Text(f);
					} else {
						value = ctx.Text(f);
					}
				}
				if (feature == "format" && !value.empty()) {
					format = value;
				}
			}
		}
		if (format.empty() || !have_block) {
			ctx.Diag("warning", "unsupported-media", "only @media (format: X) is supported; skipped " + ctx.Text(ms),
			         ms);
			return;
		}
		Statements(ctx, block, format);
	}

	void Statements(TsCtx &ctx, TSNode parent, const string &media) {
		for (uint32_t i = 0; i < ts_node_named_child_count(parent); i++) {
			auto c = ts_node_named_child(parent, i);
			auto t = NodeType(c);
			if (t == "rule_set") {
				RuleSet(ctx, c, media);
			} else if (t == "media_statement") {
				if (!media.empty()) {
					ctx.Diag("warning", "nested-media", "nested @media is not supported", c);
				} else {
					Media(ctx, c);
				}
			} else if (t == "import_statement") {
				ctx.Diag("warning", "import-unsupported", "@import is not supported yet", c);
			} else if (t == "at_rule") {
				ctx.Diag("warning", "not-yet", "this at-rule is not supported yet: " + ctx.Text(c).substr(0, 40), c);
			} else if (t == "comment" || t == "js_comment") {
				continue;
			} else if (t == "ERROR") {
				ctx.Diag("error", "syntax", "cannot parse: " + Trim40(ctx.Text(c)), c);
			} else if (t == "declaration") {
				ctx.Diag("error", "syntax", "a declaration outside a rule: " + Trim40(ctx.Text(c)), c);
			} else {
				ctx.Diag("warning", "unsupported-at-rule", "not supported: " + Trim40(ctx.Text(c)), c);
			}
		}
	}

	static string Trim40(const string &s) {
		auto t = s.substr(0, 40);
		auto nl = t.find('\n');
		return nl == string::npos ? t : t.substr(0, nl);
	}

	void Run() {
		for (auto &chunk : SplitTopLevel(text)) {
			string src = text.substr(chunk.first, chunk.second - chunk.first);
			if (Blank(src)) {
				continue;
			}
			TsTree tree(src);
			TsCtx ctx {src, chunk.first, &lines, SourcePos(), sheet.diagnostics};
			auto root = tree.Root();
			if (NodeType(root) == "ERROR") {
				ctx.Diag("error", "syntax", "cannot parse: " + Trim40(Trim(src)), root);
				continue;
			}
			Statements(ctx, root, "");
		}
	}

	static string Trim(const string &s) {
		auto b = s.find_first_not_of(" \t\r\n");
		return b == string::npos ? string() : s.substr(b);
	}
};

} // namespace

Sheet ParseSheet(const string &text) {
	Builder b(text);
	b.Run();
	return std::move(b.sheet);
}

} // namespace dmb
} // namespace duckdb
