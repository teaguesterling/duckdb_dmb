#include "css_tree.hpp"

#include "re2/re2.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <set>

namespace duckdb {
namespace dmb {

// ---------------------------------------------------------------------------
// tree-sitter plumbing
// ---------------------------------------------------------------------------

LineIndex::LineIndex(const string &text_p) : text(text_p) {
	starts.push_back(0);
	for (uint32_t i = 0; i < text.size(); i++) {
		if (text[i] == '\n') {
			starts.push_back(i + 1);
		}
	}
}

SourcePos LineIndex::Pos(uint32_t byte) const {
	auto it = std::upper_bound(starts.begin(), starts.end(), byte);
	auto line = static_cast<int32_t>(it - starts.begin()); // 1-based
	uint32_t start = starts[line - 1];
	int32_t column = 1;
	for (uint32_t i = start; i < byte && i < text.size(); i++) {
		// count code points: every byte that is not a UTF-8 continuation byte
		if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
			column++;
		}
	}
	return SourcePos {line, column};
}

TsTree::TsTree(const string &text) {
	parser = ts_parser_new();
	ts_parser_set_language(parser, tree_sitter_css());
	tree = ts_parser_parse_string(parser, nullptr, text.c_str(), static_cast<uint32_t>(text.size()));
}

TsTree::~TsTree() {
	ts_tree_delete(tree);
	ts_parser_delete(parser);
}

TSNode TsTree::Root() const {
	return ts_tree_root_node(tree);
}

string TsCtx::Text(TSNode n) const {
	auto s = ts_node_start_byte(n);
	auto e = ts_node_end_byte(n);
	if (s >= src.size() || e <= s) {
		return string();
	}
	return src.substr(s, MinValue<idx_t>(e, src.size()) - s);
}

SourcePos TsCtx::Pos(TSNode n) const {
	if (!lines) {
		return fixed;
	}
	return lines->Pos(base + ts_node_start_byte(n));
}

void TsCtx::Diag(const string &severity, const string &code, const string &message, TSNode n) {
	diagnostics.push_back(Diagnostic {severity, code, message, Pos(n)});
}

string NodeType(TSNode n) {
	return ts_node_type(n);
}

// ---------------------------------------------------------------------------
// The pseudo-classes dmb knows. Anything else is refused by name (duckent).
// ---------------------------------------------------------------------------

static const std::set<string> &NoArgPseudo() {
	static const std::set<string> s {"first-child", "last-child", "only-child", "first-of-type", "last-of-type",
	                                 "only-of-type", "empty",       "root",       "heading",       "meta",
	                                 "body",        "chrome",      "main",       "caption",       "footnote",
	                                 "callout",     "data",        "image-only", "link-only",     "external",
	                                 "internal"};
	return s;
}
static const std::set<string> &SelectorArgPseudo() {
	static const std::set<string> s {"not", "has"};
	return s;
}
static const std::set<string> &StringArgPseudo() {
	static const std::set<string> s {"contains", "matches"};
	return s;
}
static const std::set<string> &WordArgPseudo() {
	static const std::set<string> s {"h", "callout", "lang"};
	return s;
}
static const std::set<string> &NthPseudo() {
	static const std::set<string> s {"nth-child", "nth-of-type", "nth-last-child", "nth-last-of-type"};
	return s;
}

static bool IsSelectorNode(const string &t) {
	return t == "tag_name" || t == "universal_selector" || t == "class_selector" || t == "id_selector" ||
	       t == "attribute_selector" || t == "pseudo_class_selector" || t == "pseudo_element_selector" ||
	       t == "descendant_selector" || t == "child_selector" || t == "sibling_selector" ||
	       t == "adjacent_sibling_selector";
}

static string Trim(const string &s) {
	auto b = s.find_first_not_of(" \t\r\n");
	if (b == string::npos) {
		return string();
	}
	auto e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

static string Lower(string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
	return s;
}

//! The text between an `arguments` node's parentheses.
static string InnerArgs(TsCtx &ctx, TSNode args) {
	auto t = ctx.Text(args);
	if (t.size() >= 2 && t.front() == '(' && t.back() == ')') {
		t = t.substr(1, t.size() - 2);
	}
	return Trim(t);
}

static string Unquote(const string &s) {
	if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) {
		return s.substr(1, s.size() - 2);
	}
	return s;
}

static bool ParseNth(const string &raw, int32_t &a, int32_t &b) {
	string s;
	for (char c : Lower(raw)) {
		if (c != ' ') {
			s += c;
		}
	}
	if (s == "odd") {
		a = 2, b = 1;
		return true;
	}
	if (s == "even") {
		a = 2, b = 0;
		return true;
	}
	auto parse_int = [](const string &t, int32_t &v) {
		if (t.empty()) {
			return false;
		}
		size_t i = (t[0] == '+' || t[0] == '-') ? 1 : 0;
		if (i == t.size()) {
			return false;
		}
		for (size_t j = i; j < t.size(); j++) {
			if (!std::isdigit(static_cast<unsigned char>(t[j]))) {
				return false;
			}
		}
		errno = 0;
		char *end = nullptr;
		long parsed = std::strtol(t.c_str(), &end, 10);
		if (errno == ERANGE || parsed > INT32_MAX || parsed < INT32_MIN) {
			return false; // reported as bad-nth, never thrown (ParseSheet must not throw)
		}
		v = static_cast<int32_t>(parsed);
		return true;
	};
	auto n = s.find('n');
	if (n == string::npos) {
		a = 0;
		return parse_int(s, b);
	}
	auto ap = s.substr(0, n);
	if (ap.empty() || ap == "+") {
		a = 1;
	} else if (ap == "-") {
		a = -1;
	} else if (!parse_int(ap, a)) {
		return false;
	}
	auto bp = s.substr(n + 1);
	if (bp.empty()) {
		b = 0;
		return true;
	}
	return parse_int(bp, b);
}

static bool CompileInto(TsCtx &ctx, TSNode n, ComplexSelector &out, string &pe);

//! A selector argument (of :not, :has) given as a tree node.
static bool CompileArg(TsCtx &ctx, TSNode n, ComplexSelector &out) {
	string pe;
	if (!CompileInto(ctx, n, out, pe)) {
		return false;
	}
	if (!pe.empty()) {
		ctx.Diag("error", "pseudo-element-in-argument", "a pseudo-element can't be used inside a pseudo-class", n);
		return false;
	}
	out.specificity = ComputeSpecificity(out);
	return true;
}

static bool CompilePseudoClass(TsCtx &ctx, TSNode node, TSNode name_node, TSNode *args, SimpleSelector &simple) {
	simple.kind = SimpleSelector::PSEUDO_CLASS;
	simple.name = Lower(ctx.Text(name_node));
	auto &name = simple.name;
	bool has_args = args != nullptr;

	if (SelectorArgPseudo().count(name)) {
		if (!has_args) {
			ctx.Diag("error", "missing-argument", ":" + name + "() needs a selector", node);
			return false;
		}
		bool all_selectors = true;
		for (uint32_t i = 0; i < ts_node_named_child_count(*args); i++) {
			auto c = ts_node_named_child(*args, i);
			if (!IsSelectorNode(NodeType(c))) {
				all_selectors = false;
			}
		}
		if (all_selectors) {
			for (uint32_t i = 0; i < ts_node_named_child_count(*args); i++) {
				ComplexSelector arg;
				if (!CompileArg(ctx, ts_node_named_child(*args, i), arg)) {
					return false;
				}
				simple.args.push_back(std::move(arg));
			}
			if (simple.args.empty()) {
				ctx.Diag("error", "missing-argument", ":" + name + "() needs a selector", node);
				return false;
			}
			return true;
		}
		// tree-sitter parsed the argument as a value: re-parse its text as a selector (plan D2)
		ComplexSelector arg;
		string pe;
		if (!CompileSelectorText(InnerArgs(ctx, *args), arg, pe, ctx.diagnostics, ctx.Pos(*args))) {
			return false;
		}
		if (!pe.empty()) {
			ctx.Diag("error", "pseudo-element-in-argument", ":" + name + "() can't take a pseudo-element", node);
			return false;
		}
		simple.args.push_back(std::move(arg));
		return true;
	}
	if (name == "section") {
		if (!has_args) {
			ctx.Diag("error", "missing-argument", ":section() needs a heading selector", node);
			return false;
		}
		ComplexSelector arg;
		string pe;
		if (!CompileSelectorText(InnerArgs(ctx, *args), arg, pe, ctx.diagnostics, ctx.Pos(*args))) {
			return false;
		}
		if (!pe.empty()) {
			ctx.Diag("error", "pseudo-element-in-argument", ":section() can't take a pseudo-element", node);
			return false;
		}
		simple.args.push_back(std::move(arg));
		return true;
	}
	if (StringArgPseudo().count(name)) {
		if (!has_args) {
			ctx.Diag("error", "missing-argument", ":" + name + "() needs a string", node);
			return false;
		}
		simple.text_arg = Unquote(InnerArgs(ctx, *args));
		if (name == "matches") {
			duckdb_re2::RE2 re(simple.text_arg, duckdb_re2::RE2::Quiet);
			if (!re.ok()) {
				ctx.Diag("error", "bad-regex", ":matches(): " + re.error(), node);
				return false;
			}
		}
		return true;
	}
	if (NthPseudo().count(name)) {
		if (!has_args || !ParseNth(InnerArgs(ctx, *args), simple.a, simple.b)) {
			ctx.Diag("error", "bad-nth", ":" + name + "() needs an+b, odd or even", node);
			return false;
		}
		return true;
	}
	if (WordArgPseudo().count(name) && has_args) {
		simple.text_arg = Lower(Unquote(InnerArgs(ctx, *args)));
		auto &t = simple.text_arg;
		if (name == "h" && !(t.size() == 1 && t[0] >= '1' && t[0] <= '6')) {
			ctx.Diag("error", "bad-argument", ":h() takes a heading rank, 1 to 6, not " + t, node);
			return false;
		}
		return true;
	}
	if (name == "external" || name == "internal") {
		// they need the document's host, which dmb doesn't know: refused when compiled, not when matched
		ctx.Diag("warning", "not-yet", "not-yet: dmb does not match :" + name + " yet", node);
		return false;
	}
	if (NoArgPseudo().count(name) && !has_args) {
		return true;
	}
	if (WordArgPseudo().count(name) || NoArgPseudo().count(name)) {
		ctx.Diag("error", "bad-argument", ":" + name + (has_args ? " takes no argument" : "() needs an argument"),
		         node);
		return false;
	}
	ctx.Diag("error", "unknown-pseudo-class", "unknown pseudo-class :" + name, node);
	return false;
}

//! A suffix selector (.x, #x, [x], :x, ::x) applied to an optional base selector.
static bool CompileSuffix(TsCtx &ctx, TSNode n, const string &type, ComplexSelector &out, string &pe) {
	uint32_t count = ts_node_child_count(n);
	uint32_t marker = count;
	for (uint32_t i = 0; i < count; i++) {
		auto c = ts_node_child(n, i);
		if (!ts_node_is_named(c)) {
			auto t = NodeType(c);
			if (t == "." || t == "#" || t == "[" || t == ":" || t == "::") {
				marker = i;
				break;
			}
		}
	}
	if (marker == count) {
		ctx.Diag("error", "syntax", "cannot read selector " + ctx.Text(n), n);
		return false;
	}
	// the base: whatever comes before the marker
	bool has_base = false;
	for (uint32_t i = 0; i < marker; i++) {
		auto c = ts_node_child(n, i);
		if (ts_node_is_named(c)) {
			if (!CompileInto(ctx, c, out, pe)) {
				return false;
			}
			has_base = true;
		}
	}
	if (!has_base) {
		out.compounds.emplace_back();
	}
	if (!pe.empty()) {
		ctx.Diag("error", "pseudo-element-not-last", "a pseudo-element must end its selector", n);
		return false;
	}
	vector<TSNode> after;
	TSNode *args = nullptr;
	TSNode args_node;
	for (uint32_t i = marker + 1; i < count; i++) {
		auto c = ts_node_child(n, i);
		if (NodeType(c) == "arguments") {
			args_node = c;
			args = &args_node;
		} else {
			after.push_back(c);
		}
	}
	SimpleSelector simple;
	if (type == "pseudo_element_selector") {
		TSNode name_node = after.empty() ? n : after[0];
		auto name = Lower(ctx.Text(name_node));
		if (name != "before" && name != "after") {
			ctx.Diag("error", "unknown-pseudo-element", "unknown pseudo-element ::" + name, n);
			return false;
		}
		pe = name;
		return true;
	}
	if (after.empty()) {
		ctx.Diag("error", "syntax", "cannot read selector " + ctx.Text(n), n);
		return false;
	}
	if (type == "class_selector") {
		simple.kind = SimpleSelector::CLASS;
		simple.name = ctx.Text(after[0]);
	} else if (type == "id_selector") {
		simple.kind = SimpleSelector::ID;
		simple.name = ctx.Text(after[0]);
	} else if (type == "attribute_selector") {
		simple.kind = SimpleSelector::ATTR;
		bool seen_name = false;
		for (auto &c : after) {
			auto t = NodeType(c);
			if (t == "]") {
				continue;
			}
			if (!seen_name && t == "attribute_name") {
				simple.name = ctx.Text(c);
				seen_name = true;
			} else if (!ts_node_is_named(c)) {
				simple.op = t;
			} else {
				simple.value = Unquote(ctx.Text(c));
			}
		}
		static const std::set<string> ops {"", "=", "~=", "^=", "$=", "*=", "|="};
		if (!seen_name || !ops.count(simple.op)) {
			ctx.Diag("error", "syntax", "cannot read attribute selector " + ctx.Text(n), n);
			return false;
		}
	} else { // pseudo_class_selector
		if (!CompilePseudoClass(ctx, n, after[0], args, simple)) {
			return false;
		}
	}
	out.compounds.back().parts.push_back(std::move(simple));
	return true;
}

//! Bounds CompileInto's recursion: a long descendant chain nests one level per compound.
struct DepthGuard {
	static int &Depth() {
		static thread_local int depth = 0;
		return depth;
	}
	DepthGuard() {
		Depth()++;
	}
	~DepthGuard() {
		Depth()--;
	}
	bool TooDeep() const {
		return Depth() > MAX_SELECTOR_DEPTH;
	}
};

static bool CompileInto(TsCtx &ctx, TSNode n, ComplexSelector &out, string &pe) {
	DepthGuard guard;
	if (guard.TooDeep()) {
		if (guard.Depth() == MAX_SELECTOR_DEPTH + 1) {
			ctx.Diag("error", "syntax", "selector nested deeper than " + std::to_string(MAX_SELECTOR_DEPTH) + " levels",
			         n);
		}
		return false;
	}
	auto type = NodeType(n);
	if (type == "tag_name") {
		Compound c;
		SimpleSelector s;
		s.kind = SimpleSelector::TYPE;
		s.name = Lower(ctx.Text(n));
		c.parts.push_back(std::move(s));
		out.compounds.push_back(std::move(c));
		return true;
	}
	if (type == "universal_selector") {
		Compound c;
		SimpleSelector s;
		s.kind = SimpleSelector::UNIVERSAL;
		c.parts.push_back(std::move(s));
		out.compounds.push_back(std::move(c));
		return true;
	}
	if (type == "class_selector" || type == "id_selector" || type == "attribute_selector" ||
	    type == "pseudo_class_selector" || type == "pseudo_element_selector") {
		return CompileSuffix(ctx, n, type, out, pe);
	}
	char comb = 0;
	if (type == "descendant_selector") {
		comb = ' ';
	} else if (type == "child_selector") {
		comb = '>';
	} else if (type == "sibling_selector") {
		comb = '~';
	} else if (type == "adjacent_sibling_selector") {
		comb = '+';
	}
	if (comb) {
		vector<TSNode> parts;
		for (uint32_t i = 0; i < ts_node_named_child_count(n); i++) {
			parts.push_back(ts_node_named_child(n, i));
		}
		if (parts.size() == 1 && out.compounds.empty()) {
			out.leading = comb; // relative selector: :has(> img)
			return CompileInto(ctx, parts[0], out, pe);
		}
		if (parts.size() != 2) {
			ctx.Diag("error", "syntax", "cannot read selector " + ctx.Text(n), n);
			return false;
		}
		if (!CompileInto(ctx, parts[0], out, pe)) {
			return false;
		}
		if (!pe.empty()) {
			ctx.Diag("error", "pseudo-element-not-last", "a pseudo-element must end its selector", n);
			return false;
		}
		ComplexSelector right;
		if (!CompileInto(ctx, parts[1], right, pe) || right.leading) {
			return false;
		}
		out.combinators.push_back(comb);
		for (idx_t i = 0; i < right.compounds.size(); i++) {
			if (i > 0) {
				out.combinators.push_back(right.combinators[i - 1]);
			}
			out.compounds.push_back(std::move(right.compounds[i]));
		}
		return true;
	}
	if (type == "nesting_selector") {
		ctx.Diag("error", "unsupported-selector", "the nesting selector & is not supported", n);
		return false;
	}
	ctx.Diag("error", "syntax", "cannot read selector " + ctx.Text(n), n);
	return false;
}

bool CompileSelectorNode(TsCtx &ctx, TSNode node, ComplexSelector &out, string &pseudo_element) {
	if (ts_node_has_error(node)) {
		ctx.Diag("error", "syntax", "cannot parse selector " + ctx.Text(node), node);
		return false;
	}
	if (!CompileInto(ctx, node, out, pseudo_element)) {
		return false;
	}
	if (out.leading) {
		ctx.Diag("error", "syntax", "a selector can't start with a combinator", node);
		return false;
	}
	out.specificity = ComputeSpecificity(out);
	return true;
}

bool CompileSelectorText(const string &text, ComplexSelector &out, string &pseudo_element,
                         vector<Diagnostic> &diagnostics, SourcePos pos) {
	if (NestingTooDeep(text)) {
		diagnostics.push_back(Diagnostic {"error", "syntax", NestingMessage(), pos});
		return false;
	}
	string src = text + " {}";
	TsTree tree(src);
	TsCtx ctx {src, 0, nullptr, pos, diagnostics};
	auto root = tree.Root();
	TSNode selectors;
	bool found = false;
	if (ts_node_named_child_count(root) == 1) {
		auto rs = ts_node_named_child(root, 0);
		if (NodeType(rs) == "rule_set" && ts_node_named_child_count(rs) >= 1) {
			selectors = ts_node_named_child(rs, 0);
			found = NodeType(selectors) == "selectors";
		}
	}
	if (!found || ts_node_has_error(root)) {
		diagnostics.push_back(Diagnostic {"error", "syntax", "cannot parse selector " + text, pos});
		return false;
	}
	if (ts_node_named_child_count(selectors) != 1) {
		diagnostics.push_back(Diagnostic {"error", "selector-list", "expected one selector, not a list: " + text, pos});
		return false;
	}
	return CompileSelectorNode(ctx, ts_node_named_child(selectors, 0), out, pseudo_element);
}

Specificity ComputeSpecificity(const ComplexSelector &sel) {
	Specificity s;
	for (auto &compound : sel.compounds) {
		for (auto &part : compound.parts) {
			switch (part.kind) {
			case SimpleSelector::ID:
				s.a++;
				break;
			case SimpleSelector::CLASS:
			case SimpleSelector::ATTR:
				s.b++;
				break;
			case SimpleSelector::TYPE:
				s.c++;
				break;
			case SimpleSelector::UNIVERSAL:
				break;
			case SimpleSelector::PSEUDO_CLASS:
				if (part.name == "not" || part.name == "has") {
					Specificity best;
					for (auto &arg : part.args) {
						auto as = ComputeSpecificity(arg);
						if (best < as) {
							best = as;
						}
					}
					s += best;
				} else {
					s.b++;
				}
				break;
			}
		}
	}
	return s;
}

} // namespace dmb
} // namespace duckdb
