#include "apply.hpp"
#include "matcher.hpp"
#include "values.hpp"

#include "duckdb/common/error_data.hpp"
#include "re2/re2.h"
#include "utf8proc.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <unordered_map>

namespace duckdb {
namespace dmb {

namespace {

const std::set<string> &ReservedAttributes() {
	static const std::set<string> s {"heading_level", "class", "id"};
	return s;
}

//! Properties the spec defines that later tasks implement. Diagnosed as not-yet, never silently dropped.
const std::set<string> &LaterProperties() {
	static const std::set<string> s {"text-overflow", "-dcs-link"};
	return s;
}

bool IsPhase1(const string &p) {
	return p == "-dcs-id" || p == "-dcs-class";
}

bool IsPhase2(const string &p) {
	return p == "display" || p == "-dcs-prune" || p == "-dcs-dedupe";
}

bool IsPhase3(const string &p) {
	return p == "display" || p == "-dcs-wrap" || p == "-dcs-as" || p == "-dcs-level";
}

//! -dcs-level: "+n", "-n" or "n". False when it is none of those.
bool ParseLevel(const string &v, bool &relative, int &n) {
	if (v.empty() || v.size() > 4) {
		return false;
	}
	size_t i = (v[0] == '+' || v[0] == '-') ? 1 : 0;
	if (i == v.size()) {
		return false;
	}
	for (size_t j = i; j < v.size(); j++) {
		if (!std::isdigit(static_cast<unsigned char>(v[j]))) {
			return false;
		}
	}
	relative = i == 1;
	n = std::stoi(v.substr(i)) * (v[0] == '-' ? -1 : 1); // at most 3 digits: cannot overflow
	return true;
}

//! -dcs-as: "hN", "table", or an element_type name.
bool ValidAs(const string &v) {
	if (v.empty()) {
		return false;
	}
	for (char c : v) {
		if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_')) {
			return false;
		}
	}
	return true;
}

//! -dcs-wrap: blockquote, details, or callout(x). Gives the wrapper's element_type and class.
bool ParseWrap(const string &v, string &type, string &cls) {
	if (v == "blockquote") {
		type = "blockquote";
		return true;
	}
	if (v == "details") {
		type = "div";
		cls = "details";
		return true;
	}
	if (v.compare(0, 8, "callout(") == 0 && v.back() == ')') {
		auto kind = v.substr(8, v.size() - 9);
		while (!kind.empty() && kind.front() == ' ') {
			kind.erase(0, 1);
		}
		while (!kind.empty() && kind.back() == ' ') {
			kind.pop_back();
		}
		if (kind.empty() || kind.find(' ') != string::npos) {
			return false;
		}
		type = "div";
		cls = "callout " + kind;
		return true;
	}
	return false;
}

//! A row phase 3 inserts: a wrapper container before a block.
struct Inserted {
	string element_type;
	string cls;
	int32_t level;
};

//! Phase 3's result, per input row.
struct Structure {
	vector<int32_t> delta;           // level change
	vector<bool> unwrapped;          // display: contents removed this row (its subtree stays)
	vector<string> element_type;     // "" = unchanged
	vector<vector<Inserted>> before; // wrappers inserted before the row
	bool changed = false;
};

//! Element properties of phase 4.
bool IsPhase4Element(const string &p) {
	return p == "content" || p == "counter-reset" || p == "counter-increment" || p == "text-transform" ||
	       p == "-dcs-replace";
}

//! Properties a ::before or ::after rule may set.
bool IsPseudoProperty(const string &p) {
	return p == "content" || p == "-dcs-content-as" || p == "-dcs-content-place";
}

//! The source text of each top-level text(...) argument in a value, in order, with its quotes.
//! The tokenizer keeps only token text, which loses a selector's own arguments and quotes.
vector<string> TextArgSources(const string &v) {
	vector<string> out;
	char quote = 0;
	int depth = 0;
	for (idx_t i = 0; i < v.size(); i++) {
		char c = v[i];
		if (quote) {
			if (c == '\\') {
				i++;
			} else if (c == quote) {
				quote = 0;
			}
			continue;
		}
		bool word_before =
		    i > 0 && (std::isalnum(static_cast<unsigned char>(v[i - 1])) || v[i - 1] == '-' || v[i - 1] == '_');
		if (depth == 0 && !word_before && v.compare(i, 5, "text(") == 0) {
			idx_t j = i + 5;
			int inner = 1;
			char q = 0;
			for (; j < v.size() && inner > 0; j++) {
				char d = v[j];
				if (q) {
					if (d == '\\') {
						j++;
					} else if (d == q) {
						q = 0;
					}
				} else if (d == '"' || d == '\'') {
					q = d;
				} else if (d == '(') {
					inner++;
				} else if (d == ')') {
					inner--;
				}
			}
			out.push_back(v.substr(i + 5, j - 1 - (i + 5)));
			i = j - 1;
		} else if (c == '"' || c == '\'') {
			quote = c;
		} else if (c == '(') {
			depth++;
		} else if (c == ')' && depth > 0) {
			depth--;
		}
	}
	return out;
}

bool IsInteger(const string &s) {
	if (s.empty() || s.size() > 18) {
		return false;
	}
	size_t i = (s[0] == '+' || s[0] == '-') ? 1 : 0;
	if (i == s.size()) {
		return false;
	}
	for (; i < s.size(); i++) {
		if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
			return false;
		}
	}
	return true;
}

//! The spec's format names: md is markdown, restructured is rst.
string NormalizeFormat(string f) {
	std::transform(f.begin(), f.end(), f.begin(), [](unsigned char c) { return std::tolower(c); });
	if (f == "md") {
		return "markdown";
	}
	if (f == "restructured" || f == "restructuredtext") {
		return "rst";
	}
	return f;
}

//! Formats a dcs_parse_* wrapper can parse (D9): a per-row reader exists for them.
bool HasReader(const string &format) {
	return format == "text" || format == "markdown" || format == "html";
}

//! Blocks that never hold inline children.
bool LeafOnly(const string &type) {
	static const std::set<string> s {"code", "table", "image", "hr", "page_break", "metadata",
	                                 "raw",  "deflist", "lineblock", "document"};
	return s.count(type) > 0;
}

//! A row phase 4 generates.
struct Gen {
	string kind;
	string element_type;
	string content;
	std::map<string, string> attrs;
};

//! Phase 4's result, per input row.
struct Content {
	vector<bool> replaced;       // content was replaced; the row's inline descendants are dropped
	vector<string> new_content;
	vector<vector<Gen>> before_siblings, after_siblings, first_children, last_children, materialized;
	vector<bool> has_transform, has_replace;
	vector<string> transform, replace_re, replace_with;
	bool changed = false;
	void Resize(idx_t n) {
		replaced.assign(n, false);
		new_content.assign(n, "");
		before_siblings.resize(n);
		after_siblings.resize(n);
		first_children.resize(n);
		last_children.resize(n);
		materialized.resize(n);
		has_transform.assign(n, false);
		has_replace.assign(n, false);
		transform.assign(n, "");
		replace_re.assign(n, "");
		replace_with.assign(n, "");
	}
};

//! Apply text-transform to one string: uppercase, lowercase, capitalize (first letter of each word).
string TextTransform(const string &text, const string &how) {
	if (how != "uppercase" && how != "lowercase" && how != "capitalize") {
		return text;
	}
	auto data = reinterpret_cast<const utf8proc_uint8_t *>(text.data());
	auto len = static_cast<utf8proc_ssize_t>(text.size());
	string out;
	bool word_start = true;
	for (utf8proc_ssize_t pos = 0; pos < len;) {
		utf8proc_int32_t cp;
		auto n = utf8proc_iterate(data + pos, len - pos, &cp);
		if (n <= 0) {
			out += text[pos++];
			continue;
		}
		pos += n;
		auto cat = utf8proc_category(cp);
		bool letter = cat >= UTF8PROC_CATEGORY_LU && cat <= UTF8PROC_CATEGORY_LO;
		bool digit = cat >= UTF8PROC_CATEGORY_ND && cat <= UTF8PROC_CATEGORY_NO;
		if (how == "uppercase") {
			cp = utf8proc_toupper(cp);
		} else if (how == "lowercase") {
			cp = utf8proc_tolower(cp);
		} else if (letter && word_start) {
			cp = utf8proc_totitle(cp);
		}
		word_start = !(letter || digit);
		utf8proc_uint8_t buf[4];
		auto w = utf8proc_encode_char(cp, buf);
		out.append(reinterpret_cast<const char *>(buf), static_cast<size_t>(w));
	}
	return out;
}

bool IsCustom(const string &p) {
	return p.size() > 2 && p[0] == '-' && p[1] == '-';
}

//! Phase 7: any other CSS property is presentation, carried in attributes['style'].
bool IsPresentation(const string &p) {
	return !IsCustom(p) && p.compare(0, 5, "-dcs-") != 0 && !IsPhase1(p) && !IsPhase2(p) && !IsPhase3(p) &&
	       !IsPhase4Element(p) && !IsPseudoProperty(p) && p != "order" && !LaterProperties().count(p);
}

//! A YAML double-quoted scalar.
string YamlQuote(const string &v) {
	string out = "\"";
	for (char c : v) {
		if (c == '"' || c == '\\') {
			out += '\\';
			out += c;
		} else if (c == '\n') {
			out += "\\n";
		} else if (c == '\t') {
			out += "\\t";
		} else {
			out += c;
		}
	}
	return out + "\"";
}

//! Rows with at least one inline child, found in one pass (a per-row subtree scan is quadratic
//! on a deeply nested document).
vector<bool> InlineParents(const BlockTree &tree) {
	vector<bool> out(tree.nodes.size(), false);
	for (auto &n : tree.nodes) {
		if (n.kind == "inline" && n.parent >= 0) {
			out[n.parent] = true;
		}
	}
	return out;
}

struct Winner {
	const Rule *rule = nullptr;
	const Declaration *decl = nullptr;
};

bool Beats(const Rule &r, const Declaration &d, const Winner &w) {
	if (!w.decl) {
		return true;
	}
	if (d.important != w.decl->important) {
		return d.important;
	}
	auto spec = [](const Rule &rule) {
		auto s = rule.selector.specificity;
		if (!rule.pseudo_element.empty()) {
			s.c++;
		}
		return s;
	};
	auto a = spec(r), b = spec(*w.rule);
	if (a < b) {
		return false;
	}
	if (b < a) {
		return true;
	}
	return d.order >= w.decl->order;
}

Value OptStr(const std::map<string, string> &attrs, const string &key) {
	auto it = attrs.find(key);
	return it == attrs.end() ? Value(LogicalType::VARCHAR) : Value(it->second);
}

struct Engine {
	const Sheet &sheet;
	ApplyResult result;
	std::set<int32_t> reported_rules; // a rule's not-yet diagnostic is reported once

	explicit Engine(const Sheet &s) : sheet(s) {
		result.diagnostics = s.diagnostics;
	}

	bool as_table_warned = false;
	std::set<int32_t> value_reported; // declarations whose value error is already a diagnostic

	void Diag(const string &severity, const string &code, const string &message, SourcePos pos) {
		result.diagnostics.push_back(Diagnostic {severity, code, message, pos});
	}

	//! Per node, per property: the winning declaration among the rules that pass `want`.
	template <class WANT>
	vector<std::map<string, Winner>> Cascade(const BlockTree &tree, WANT want, const string &pseudo = "") {
		vector<std::map<string, Winner>> out(tree.nodes.size());
		Matcher matcher(tree);
		for (auto &rule : sheet.rules) {
			if (!rule.usable || !rule.media_format.empty() || rule.pseudo_element != pseudo) {
				continue;
			}
			bool any = false;
			for (auto &d : rule.declarations) {
				any = any || want(d.property);
			}
			if (!any) {
				continue;
			}
			vector<idx_t> hits;
			try {
				for (idx_t i = 0; i < tree.nodes.size(); i++) {
					if (matcher.Matches(rule.selector, i)) {
						hits.push_back(i);
					}
				}
			} catch (NotImplementedException &e) {
				if (reported_rules.insert(rule.index).second) {
					Diag("warning", "not-yet", ErrorData(e).RawMessage(), rule.pos);
				}
				continue;
			}
			for (auto i : hits) {
				for (auto &d : rule.declarations) {
					if (!want(d.property)) {
						continue;
					}
					auto &w = out[i][d.property];
					if (Beats(rule, d, w)) {
						w = Winner {&rule, &d};
					}
				}
			}
		}
		return out;
	}

	//! Sheet-level checks that do not depend on matching: each declaration once.
	//! A function name the value evaluator doesn't know, searched through every argument; "" if none.
	static string UnknownFunction(const vector<Token> &tokens) {
		static const std::set<string> known {"text", "attr", "slug", "counter", "url"};
		for (auto &tok : tokens) {
			if (tok.kind != Token::FUNCTION) {
				continue;
			}
			if (!known.count(tok.text)) {
				return tok.text;
			}
			for (auto &arg : tok.args) {
				auto inner = UnknownFunction(arg);
				if (!inner.empty()) {
					return inner;
				}
			}
		}
		return "";
	}

	//! The value checks no document can change: a value that doesn't tokenize, or an unknown
	//! function. Before these ran here, they were found only when a rule matched a block (M4).
	void CheckValue(const Declaration &d) {
		auto &p = d.property;
		auto v = Trim(d.value_text);
		bool evaluated = (p == "-dcs-id" && v != "auto") || (IsCustom(p) && v != "unset") ||
		                 (p == "content" && v != "none" && v != "normal");
		if (!evaluated) {
			return;
		}
		vector<Token> tokens;
		string error;
		if (!TokenizeValue(d.value_text, tokens, error)) {
			Diag("error", "bad-value", p + ": " + error, d.pos);
			value_reported.insert(d.order);
			return;
		}
		auto unknown = UnknownFunction(tokens);
		if (!unknown.empty()) {
			Diag("error", "unknown-function", p + ": unknown-function: " + unknown + "()", d.pos);
			value_reported.insert(d.order);
		}
	}

	void CheckDeclarations() {
		std::set<int32_t> seen;
		for (auto &rule : sheet.rules) {
			if (!rule.media_format.empty()) {
				Diag("warning", "not-yet", "@media (format: " + rule.media_format + ") rules are not applied yet",
				     rule.pos);
			}
			for (auto &d : rule.declarations) {
				if (!seen.insert(d.order).second) {
					continue;
				}
				CheckValue(d);
				auto &p = d.property;
				if (IsCustom(p)) {
					if (ReservedAttributes().count(p.substr(2))) {
						Diag("error", "reserved-attribute",
						     p + " is reserved: set " + p.substr(2) + " through its own property", d.pos);
					}
				} else if (IsPhase1(p)) {
					// checked by CheckValue; the pseudo-element check below still applies
				} else if (IsPhase2(p) || IsPhase3(p)) {
					auto v = Trim(d.value_text);
					bool relative;
					int n;
					string wt, wc;
					if (p == "display" && v != "none" && v != "contents") {
						Diag("warning", "not-yet", "display: " + v + " is not supported yet", d.pos);
					} else if (p == "-dcs-wrap" && !ParseWrap(v, wt, wc)) {
						Diag("error", "bad-value", "-dcs-wrap takes blockquote, details or callout(x), not " + v, d.pos);
					} else if (p == "-dcs-as" && !ValidAs(v)) {
						Diag("error", "bad-value", "-dcs-as takes an element type, hN or table, not " + v, d.pos);
					} else if (p == "-dcs-level" && !ParseLevel(v, relative, n)) {
						Diag("error", "bad-value", "-dcs-level takes +n, -n or n, not " + v, d.pos);
					} else if (p == "-dcs-prune" && v != "section") {
						Diag("error", "bad-value", "-dcs-prune takes section, not " + v, d.pos);
					} else if (p == "-dcs-dedupe" && v != "text") {
						Diag("error", "bad-value", "-dcs-dedupe takes text, not " + v, d.pos);
					}
				} else if (IsPhase4Element(p) || IsPseudoProperty(p)) {
					auto v = Trim(d.value_text);
					vector<Token> rp;
					string rp_error;
					if (p == "-dcs-replace" && TokenizeValue(d.value_text, rp, rp_error) && rp.size() == 2 &&
					    rp[0].kind == Token::STRING) {
						duckdb_re2::RE2 re(rp[0].text, duckdb_re2::RE2::Quiet);
						if (!re.ok()) {
							Diag("error", "bad-regex", "-dcs-replace: " + re.error(), d.pos);
						}
					}
					if (p == "-dcs-content-place" && v != "inline" && v != "block") {
						Diag("error", "bad-value", "-dcs-content-place takes inline or block, not " + v, d.pos);
					} else if (p == "text-transform" && v != "uppercase" && v != "lowercase" && v != "capitalize" &&
					           v != "none") {
						Diag("error", "bad-value", "text-transform takes uppercase, lowercase, capitalize or none", d.pos);
					} else if (p == "-dcs-content-as" && !HasReader(NormalizeFormat(v))) {
						Diag("warning", "no-reader",
						     "no per-row reader parses " + v + "; the content stays a raw block with its format", d.pos);
					}
					if (rule.pseudo_element.empty() && (p == "-dcs-content-as" || p == "-dcs-content-place")) {
						Diag("warning", "pseudo-only", p + " applies to ::before and ::after content only", d.pos);
					}
				} else if (p == "order") {
					if (!IsInteger(Trim(d.value_text))) {
						Diag("error", "bad-value", "order takes an integer, not " + Trim(d.value_text), d.pos);
					}
				} else if (LaterProperties().count(p)) {
					Diag("warning", "not-yet", p + " is not supported yet", d.pos);
				} else if (p.compare(0, 5, "-dcs-") == 0) {
					Diag("error", "unknown-property", "unknown property " + p, d.pos);
				} // else phase 7: presentation, carried in attributes['style']
				if (!rule.pseudo_element.empty() && !IsPseudoProperty(p)) {
					Diag("warning", "not-yet",
					     "::" + rule.pseudo_element + " takes content, -dcs-content-as and -dcs-content-place only; " + p +
					         " is not supported there yet",
					     d.pos);
				}
			}
		}
	}

	bool Evaluate(const ValueEvaluator &eval, const Declaration &d, idx_t node, string &out, bool &is_null) {
		vector<Token> tokens;
		string error;
		if (!TokenizeValue(d.value_text, tokens, error) || !eval.Eval(tokens, node, out, is_null, error)) {
			auto code = error.compare(0, 16, "unknown-function") == 0 ? "unknown-function" : "bad-value";
			if (value_reported.insert(d.order).second) { // once per declaration, not once per block
				Diag("error", code, d.property + ": " + error, d.pos);
			}
			return false;
		}
		return true;
	}

	void Record(const BlockTree &tree, idx_t i, const Winner &w, const Value &before, const Value &after) {
		if (before.IsNull() || after.IsNull() ? before.IsNull() == after.IsNull()
		                                       : before.ToString() == after.ToString()) {
			return; // unchanged (Value == refuses NULL on v2.0)
		}
		result.changes.push_back(Change {tree.nodes[i].element_order, w.rule->index, w.decl->property, before, after, w.decl->pos});
	}

	void Phase1(const BlockTree &tree, vector<std::map<string, string>> &attrs) {
		auto cascade = Cascade(tree, IsPhase1);
		ValueEvaluator eval(tree);
		std::set<string> ids;
		std::unordered_map<string, int> next_suffix; // per base: the last -n tried, so k repeats cost O(k)
		for (auto &a : attrs) {
			auto it = a.find("id");
			if (it != a.end()) {
				ids.insert(it->second);
			}
		}
		for (idx_t i = 0; i < tree.nodes.size(); i++) {
			auto &a = attrs[i];
			auto idw = cascade[i].find("-dcs-id");
			if (idw != cascade[i].end()) {
				auto &w = idw->second;
				auto before = OptStr(a, "id");
				if (Trim(w.decl->value_text) == "auto") {
					if (!a.count("id")) {
						auto base = Slug(tree.Text(i));
						if (!base.empty()) {
							auto id = base;
							int &n = next_suffix[base];
							while (ids.count(id)) {
								id = base + "-" + std::to_string(++n);
							}
							ids.insert(id);
							a["id"] = id;
						}
					}
				} else {
					string v;
					bool is_null;
					if (Evaluate(eval, *w.decl, i, v, is_null)) {
						if (is_null || v.empty()) {
							a.erase("id");
						} else {
							// ids are unique in a document, however they were set (D6's -1, -2 rule)
							auto own = a.find("id");
							if (own != a.end() && own->second != v) {
								ids.erase(own->second);
							}
							auto id = v;
							int &n = next_suffix[v];
							while (ids.count(id) && !(own != a.end() && own->second == id)) {
								id = v + "-" + std::to_string(++n);
							}
							a["id"] = id;
							ids.insert(id);
						}
					}
				}
				Record(tree, i, w, before, OptStr(a, "id"));
			}
			auto clw = cascade[i].find("-dcs-class");
			if (clw != cascade[i].end()) {
				auto &w = clw->second;
				auto before = OptStr(a, "class");
				auto classes = tree.Classes(i);
				string word;
				std::istringstream in(w.decl->value_text);
				while (in >> word) {
					if (word[0] == '-' && word.size() > 1) {
						auto name = word.substr(1);
						classes.erase(std::remove(classes.begin(), classes.end(), name), classes.end());
					} else {
						auto name = word[0] == '+' ? word.substr(1) : word;
						if (!name.empty() && std::find(classes.begin(), classes.end(), name) == classes.end()) {
							classes.push_back(name);
						}
					}
				}
				string joined;
				for (auto &c : classes) {
					joined += (joined.empty() ? "" : " ") + c;
				}
				if (joined.empty()) {
					a.erase("class");
				} else {
					a["class"] = joined;
				}
				Record(tree, i, w, before, OptStr(a, "class"));
			}
		}
	}

	void Attributes(const BlockTree &tree, vector<std::map<string, string>> &attrs) {
		auto cascade = Cascade(tree, [](const string &p) { return IsCustom(p) && !ReservedAttributes().count(p.substr(2)); });
		ValueEvaluator eval(tree);
		for (idx_t i = 0; i < tree.nodes.size(); i++) {
			for (auto &entry : cascade[i]) {
				auto &w = entry.second;
				auto key = entry.first.substr(2);
				auto &a = attrs[i];
				auto before = OptStr(a, key);
				if (Trim(w.decl->value_text) == "unset") {
					a.erase(key);
				} else {
					string v;
					bool is_null;
					if (!Evaluate(eval, *w.decl, i, v, is_null)) {
						continue;
					}
					if (is_null) {
						a.erase(key);
					} else {
						a[key] = v;
					}
				}
				Record(tree, i, w, before, OptStr(a, key));
			}
		}
	}

	//! Phase 2: which rows to drop. Matches the same document as the attribute pass.
	vector<bool> Prune(const BlockTree &tree) {
		vector<bool> drop(tree.nodes.size(), false);
		auto cascade = Cascade(tree, IsPhase2);
		auto mark = [&](idx_t from, idx_t to, idx_t i, const Winner &w) {
			for (idx_t j = from; j < to; j++) {
				drop[j] = true;
			}
			result.changes.push_back(Change {tree.nodes[i].element_order, w.rule->index, w.decl->property,
			                                 Value("present"), Value(LogicalType::VARCHAR), w.decl->pos});
		};
		std::set<string> seen_texts;
		for (idx_t i = 0; i < tree.nodes.size(); i++) {
			if (drop[i]) {
				continue;
			}
			auto &props = cascade[i];
			auto display = props.find("display");
			if (display != props.end() && Trim(display->second.decl->value_text) == "none") {
				mark(i, tree.nodes[i].subtree_end, i, display->second);
				continue;
			}
			auto prune = props.find("-dcs-prune");
			if (prune != props.end() && Trim(prune->second.decl->value_text) == "section") {
				if (tree.nodes[i].element_type != "heading") {
					Diag("error", "prune-section-not-heading", "-dcs-prune: section needs a heading",
					     prune->second.decl->pos);
				} else {
					auto extent = SectionExtent(tree, i);
					mark(i, extent.back() + 1, i, prune->second);
					continue;
				}
			}
			auto dedupe = props.find("-dcs-dedupe");
			if (dedupe != props.end() && Trim(dedupe->second.decl->value_text) == "text") {
				auto text = tree.Text(i);
				if (!text.empty() && !seen_texts.insert(text).second) {
					mark(i, tree.nodes[i].subtree_end, i, dedupe->second);
				}
			}
		}
		return drop;
	}

	//! Phase 3: unwrap, wrap, retag, heading level. Matches the same document as phase 2, and
	//! skips rows phase 2 dropped.
	Structure Phase3(const BlockTree &tree, vector<std::map<string, string>> &attrs, const vector<bool> &drop) {
		Structure st;
		auto n_rows = tree.nodes.size();
		st.delta.assign(n_rows, 0);
		st.unwrapped.assign(n_rows, false);
		st.element_type.assign(n_rows, "");
		st.before.resize(n_rows);
		auto cascade = Cascade(tree, [](const string &p) { return IsPhase3(p) && p != "display"; });
		auto display = Cascade(tree, [](const string &p) { return p == "display"; });
		auto record = [&](idx_t i, const Winner &w, const string &before, const string &after) {
			result.changes.push_back(Change {tree.nodes[i].element_order, w.rule->index, w.decl->property,
			                                 Value(before), Value(after), w.decl->pos});
			st.changed = true;
		};
		// Level changes cover whole subtrees. A difference array keeps that linear: `run` is the
		// change in force at row i, `diff[k]` is what starts or ends at row k.
		auto inline_parent = InlineParents(tree);
		vector<int32_t> diff(n_rows + 1, 0);
		int32_t run = 0;
		for (idx_t i = 0; i < n_rows; i++) {
			run += diff[i];
			st.delta[i] = run;
			if (drop[i] || !tree.Matchable(i)) {
				continue;
			}
			auto &node = tree.nodes[i];
			auto &props = cascade[i];
			// display: contents
			auto dw = display[i].find("display");
			if (dw != display[i].end() && Trim(dw->second.decl->value_text) == "contents") {
				if (inline_parent[i]) {
					Diag("error", "contents-has-inlines",
					     "display: contents needs a container of blocks; this block holds inline text",
					     dw->second.decl->pos);
				} else {
					st.unwrapped[i] = true;
					diff[i + 1]--; // the descendants rise one level
					diff[node.subtree_end]++;
					record(i, dw->second, node.element_type, "unwrapped");
					continue;
				}
			}
			// -dcs-as
			auto &a = attrs[i];
			string type = node.element_type;
			auto as = props.find("-dcs-as");
			if (as != props.end()) {
				auto v = Trim(as->second.decl->value_text);
				if (!ValidAs(v)) {
					// diagnosed by CheckDeclarations
				} else if (v == "table") {
					if (type == "code" && IsDataLanguage(tree.Attr(i, "language"))) {
						a["dcs-as"] = "table"; // D7: marked for a converter
						record(i, as->second, type, "table (marked)");
						if (!as_table_warned) {
							as_table_warned = true;
							Diag("warning", "not-yet",
							     "-dcs-as: table marks the block, but the conversion waits for duckdb_panduck#188",
							     as->second.decl->pos);
						}
					} else {
						Diag("error", "as-table-needs-data",
						     "-dcs-as: table needs a code block in csv, tsv, json, jsonl or yaml", as->second.decl->pos);
					}
				} else if (v.size() == 2 && v[0] == 'h' && v[1] >= '1' && v[1] <= '6') {
					type = "heading";
					a["heading_level"] = string(1, v[1]);
					record(i, as->second, node.element_type, v);
				} else if (v != type) {
					if (type == "heading") {
						a.erase("heading_level");
					}
					if (v == "heading" && !a.count("heading_level")) {
						a["heading_level"] = "1";
					}
					type = v;
					record(i, as->second, node.element_type, v);
				}
			}
			// -dcs-level
			auto lw = props.find("-dcs-level");
			if (lw != props.end()) {
				bool relative;
				int n;
				if (!ParseLevel(Trim(lw->second.decl->value_text), relative, n)) {
					// diagnosed by CheckDeclarations
				} else if (type != "heading") {
					Diag("error", "level-not-heading", "-dcs-level needs a heading", lw->second.decl->pos);
				} else {
					auto it = a.find("heading_level");
					int rank = 1;
					if (it != a.end()) {
						rank = std::atoi(it->second.c_str());
					}
					string before = std::to_string(rank);
					int now = relative ? rank + n : n;
					now = MaxValue(1, MinValue(6, now));
					a["heading_level"] = std::to_string(now);
					if (std::to_string(now) != before) {
						record(i, lw->second, before, std::to_string(now));
					}
				}
			}
			if (type != node.element_type) {
				st.element_type[i] = type;
			}
			// -dcs-wrap: a container before the block, at its level; the block's subtree moves down
			auto ww = props.find("-dcs-wrap");
			if (ww != props.end()) {
				string wt, wc;
				if (ParseWrap(Trim(ww->second.decl->value_text), wt, wc)) {
					st.before[i].push_back(Inserted {wt, wc, node.level + run});
					run++; // the block and its subtree move down one level
					st.delta[i] = run;
					diff[node.subtree_end]--;
					record(i, ww->second, "", wt + (wc.empty() ? "" : "." + wc));
				}
			}
		}
		return st;
	}

	//! Phase 4: content. Element content replaces text; ::before and ::after generate rows.
	//! Counters run in document order: reset, then increment, then the block's own content.
	Content Phase4(const BlockTree &tree, const vector<std::map<string, string>> &attrs, const vector<bool> &drop,
	               const Structure &st) {
		Content ct;
		auto n_rows = tree.nodes.size();
		ct.Resize(n_rows);
		auto elem = Cascade(tree, IsPhase4Element);
		auto before = Cascade(tree, IsPseudoProperty, "before");
		auto after = Cascade(tree, IsPseudoProperty, "after");
		std::map<string, int64_t> counters;
		ValueEvaluator eval(tree);
		eval.SetCounters(&counters);
		auto has_block_children = [&](idx_t i) { return !tree.nodes[i].children.empty(); };
		auto inline_parent = InlineParents(tree);
		auto has_inline_children = [&](idx_t i) { return static_cast<bool>(inline_parent[i]); };
		for (idx_t i = 0; i < n_rows; i++) {
			if (drop[i] || st.unwrapped[i] || !tree.Matchable(i)) {
				continue;
			}
			auto &node = tree.nodes[i];
			auto &props = elem[i];
			// transforms, kept per block; inherited at emit time
			auto tt = props.find("text-transform");
			if (tt != props.end()) {
				ct.transform[i] = Trim(tt->second.decl->value_text);
				ct.has_transform[i] = true;
				ct.changed = true; // so Apply does not take the "nothing to do" path
			}
			auto rp = props.find("-dcs-replace");
			if (rp != props.end()) {
				vector<Token> tokens;
				string error;
				if (TokenizeValue(rp->second.decl->value_text, tokens, error) && tokens.size() == 2 &&
				    tokens[0].kind == Token::STRING && tokens[1].kind == Token::STRING) {
					ct.replace_re[i] = tokens[0].text;
					ct.replace_with[i] = tokens[1].text;
					ct.has_replace[i] = true;
					ct.changed = true;
				} else {
					Diag("error", "bad-value", "-dcs-replace takes two strings: a pattern and its replacement",
					     rp->second.decl->pos);
				}
			}
			// counters
			auto reset = props.find("counter-reset");
			if (reset != props.end()) {
				ApplyCounters(reset->second, counters, true);
			}
			auto inc = props.find("counter-increment");
			if (inc != props.end()) {
				ApplyCounters(inc->second, counters, false);
			}
			// content on the element itself replaces its text
			bool leaf_with_text = false;
			string current = node.content;
			auto cw = props.find("content");
			if (cw != props.end()) {
				auto v = Trim(cw->second.decl->value_text);
				if (v == "normal" || v == "none") {
					// CSS's keywords: leave the element as it is
				} else if (has_block_children(i)) {
					Diag("error", "content-on-container", "content replaces text; this block holds other blocks",
					     cw->second.decl->pos);
				} else {
					string out;
					bool is_null;
					if (Evaluate(eval, *cw->second.decl, i, out, is_null)) {
						ct.replaced[i] = true;
						ct.new_content[i] = is_null ? string() : out;
						current = ct.new_content[i];
						result.changes.push_back(Change {node.element_order, cw->second.rule->index, "content",
						                                 Value(tree.Text(i)), Value(current), cw->second.decl->pos});
						ct.changed = true;
					}
				}
			}
			bool inlines_remain = !ct.replaced[i] && has_inline_children(i);
			leaf_with_text = !inlines_remain && !current.empty();
			bool materialized = false;
			// ::before and ::after
			for (int side = 0; side < 2; side++) {
				auto &pseudo = side == 0 ? before[i] : after[i];
				auto content = pseudo.find("content");
				if (content == pseudo.end()) {
					continue;
				}
				auto &decl = *content->second.decl;
				auto v = Trim(decl.value_text);
				if (v == "none" || v == "normal") {
					continue;
				}
				string text;
				bool is_null;
				if (!Evaluate(eval, decl, i, text, is_null) || is_null || text.empty()) {
					continue;
				}
				string place = "inline", format = "text";
				auto pl = pseudo.find("-dcs-content-place");
				if (pl != pseudo.end()) {
					place = Trim(pl->second.decl->value_text);
				}
				auto fm = pseudo.find("-dcs-content-as");
				if (fm != pseudo.end()) {
					format = NormalizeFormat(Trim(fm->second.decl->value_text));
				}
				Gen gen;
				gen.content = text;
				if (format != "text") {
					gen.element_type = "raw";
					gen.attrs["format"] = format;
					gen.attrs["dcs-content-as"] = format;
				}
				if (place == "block") {
					gen.kind = "block";
					if (gen.element_type.empty()) {
						gen.element_type = "paragraph";
					}
					(side == 0 ? ct.before_siblings[i] : ct.after_siblings[i]).push_back(gen);
				} else {
					if (LeafOnly(node.element_type) || has_block_children(i)) {
						Diag("error", "content-needs-text-block",
						     "inline ::" + string(side == 0 ? "before" : "after") + " content needs a block of text; " +
						         node.element_type + " can't hold it (use -dcs-content-place: block)",
						     decl.pos);
						continue;
					}
					gen.kind = "inline";
					if (gen.element_type.empty()) {
						gen.element_type = "text";
					}
					if (leaf_with_text && !materialized) {
						// the block's own text becomes a child, so the children stay authoritative
						Gen own;
						own.kind = "inline";
						own.element_type = "text";
						own.content = current;
						ct.materialized[i].push_back(own);
						materialized = true;
						if (node.element_type != "heading") {
							ct.replaced[i] = true; // only a heading keeps a flattened title beside children
							ct.new_content[i] = "";
						}
					}
					(side == 0 ? ct.first_children[i] : ct.last_children[i]).push_back(gen);
				}
				result.changes.push_back(Change {node.element_order, content->second.rule->index,
				                                 side == 0 ? "::before" : "::after", Value(LogicalType::VARCHAR),
				                                 Value(text), decl.pos});
				ct.changed = true;
			}
		}
		return ct;
	}

	void ApplyCounters(const Winner &w, std::map<string, int64_t> &counters, bool reset) {
		std::istringstream in(Trim(w.decl->value_text));
		vector<string> words;
		string word;
		while (in >> word) {
			words.push_back(word);
		}
		if (words.size() == 1 && words[0] == "none") {
			return;
		}
		for (idx_t k = 0; k < words.size(); k++) {
			auto &name = words[k];
			int64_t n = reset ? 0 : 1;
			if (k + 1 < words.size() && IsInteger(words[k + 1])) {
				n = std::strtoll(words[k + 1].c_str(), nullptr, 10);
				k++;
			}
			if (reset) {
				counters[name] = n;
			} else {
				counters[name] += n;
			}
		}
	}

	//! Phase 5: order: n, per block. 0 where unset or invalid.
	vector<int64_t> OrderKeys(const BlockTree &tree, const vector<bool> &drop) {
		vector<int64_t> keys(tree.nodes.size(), 0);
		auto cascade = Cascade(tree, [](const string &p) { return p == "order"; });
		for (idx_t i = 0; i < tree.nodes.size(); i++) {
			auto it = cascade[i].find("order");
			if (drop[i] || it == cascade[i].end()) {
				continue;
			}
			auto v = Trim(it->second.decl->value_text);
			if (IsInteger(v)) {
				keys[i] = std::strtoll(v.c_str(), nullptr, 10);
				if (keys[i] != 0) {
					result.changes.push_back(Change {tree.nodes[i].element_order, it->second.rule->index, "order",
					                                 Value("0"), Value(v), it->second.decl->pos});
				}
			}
		}
		return keys;
	}

	//! Phase 7: presentation properties into attributes['style'], winners in source order,
	//! after any style the block already had (so the sheet's declarations win).
	void Style(const BlockTree &tree, vector<std::map<string, string>> &attrs, const vector<bool> &drop) {
		auto cascade = Cascade(tree, IsPresentation);
		for (idx_t i = 0; i < tree.nodes.size(); i++) {
			if (drop[i] || cascade[i].empty()) {
				continue;
			}
			vector<const Winner *> winners;
			for (auto &kv : cascade[i]) {
				winners.push_back(&kv.second);
			}
			std::sort(winners.begin(), winners.end(),
			          [](const Winner *a, const Winner *b) { return a->decl->order < b->decl->order; });
			string style;
			for (auto w : winners) {
				style += (style.empty() ? "" : "; ") + w->decl->property + ": " + Trim(w->decl->value_text);
			}
			auto &a = attrs[i];
			auto before = OptStr(a, "style");
			auto it = a.find("style");
			a["style"] = it == a.end() || Trim(it->second).empty() ? style : Trim(it->second) + "; " + style;
			result.changes.push_back(Change {tree.nodes[i].element_order, winners.back()->rule->index, "style", before,
			                                 Value(a["style"]), winners.back()->decl->pos});
		}
	}

	//! Phase 6: @frontmatter, evaluated against the document after phase 1. text(sel) is the text of
	//! the first block sel matches; strings, words and url() as elsewhere. Gives "key: value" lines.
	vector<std::pair<string, string>> Frontmatter(const BlockTree &tree) {
		vector<std::pair<string, string>> lines;
		ValueEvaluator eval(tree);
		Matcher matcher(tree);
		for (auto &d : sheet.frontmatter) {
			vector<Token> tokens;
			string error, value;
			if (!TokenizeValue(d.value_text, tokens, error)) {
				Diag("error", "bad-value", d.property + ": " + error, d.pos);
				continue;
			}
			bool ok = true, prev_word = false;
			auto text_args = TextArgSources(d.value_text);
			idx_t text_k = 0;
			for (auto &tok : tokens) {
				string piece;
				bool word = tok.kind == Token::IDENT || tok.kind == Token::NUMBER;
				if (tok.kind == Token::FUNCTION && tok.text == "text") {
					string sel_text = text_k < text_args.size() ? Trim(text_args[text_k++]) : string();
					ComplexSelector sel;
					string pe;
					vector<Diagnostic> diags;
					if (sel_text.empty() || !CompileSelectorText(sel_text, sel, pe, diags, d.pos) || !pe.empty()) {
						Diag("error", "bad-value",
						     d.property + ": text() in @frontmatter needs one selector" +
						         (diags.empty() ? string() : " (" + diags[0].message + ")"),
						     d.pos);
						ok = false;
						break;
					}
					try {
						for (idx_t i = 0; i < tree.nodes.size(); i++) {
							if (matcher.Matches(sel, i)) {
								piece = tree.Text(i);
								break;
							}
						}
					} catch (NotImplementedException &e) {
						Diag("warning", "not-yet", ErrorData(e).RawMessage(), d.pos);
						ok = false;
						break;
					}
				} else if (tok.kind == Token::FUNCTION && tok.text != "url") {
					Diag("error", "unknown-function", d.property + ": " + tok.text + "() does not work in @frontmatter",
					     d.pos);
					ok = false;
					break;
				} else {
					bool is_null = false;
					if (!eval.Eval({tok}, 0, piece, is_null, error)) {
						Diag("error", "bad-value", d.property + ": " + error, d.pos);
						ok = false;
						break;
					}
				}
				if (word && prev_word) {
					value += ' ';
				}
				value += piece;
				prev_word = word;
			}
			if (!ok) {
				continue;
			}
			for (auto &line : lines) {
				if (line.first == d.property) {
					line.second = YamlQuote(value); // a later declaration of the same key wins
					ok = false;
				}
			}
			if (ok) {
				lines.emplace_back(d.property, YamlQuote(value));
			}
			result.changes.push_back(Change {0, -1, "@frontmatter " + d.property, Value(LogicalType::VARCHAR),
			                                 Value(value), d.pos});
		}
		return lines;
	}

	static string Trim(const string &s) {
		auto b = s.find_first_not_of(" \t\r\n");
		if (b == string::npos) {
			return string();
		}
		return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
	}
};

} // namespace

static bool fields_content_null(const Value &item, const BlockFields &fields) {
	return StructValue::GetChildren(item)[fields.content].IsNull();
}

vector<Diagnostic> CheckSheet(const Sheet &sheet) {
	Engine engine(sheet);
	engine.CheckDeclarations();
	return std::move(engine.result.diagnostics);
}

ApplyResult Apply(const Value &list, const BlockFields &fields, const LogicalType &block_type, const Sheet &sheet) {
	Engine engine(sheet);
	engine.CheckDeclarations();
	auto tree = BlockTree::FromList(list, fields);
	auto n_rows = tree.nodes.size();
	vector<std::map<string, string>> attrs(n_rows);
	for (idx_t i = 0; i < n_rows; i++) {
		attrs[i] = tree.nodes[i].attributes;
	}
	// phase 1 matches the input; everything after it matches the document after phase 1
	engine.Phase1(tree, attrs);
	auto after1 = tree;
	for (idx_t i = 0; i < n_rows; i++) {
		after1.nodes[i].attributes = attrs[i];
	}
	engine.Attributes(after1, attrs);
	auto drop = engine.Prune(after1);
	auto st = engine.Phase3(after1, attrs, drop);
	auto ct = engine.Phase4(after1, attrs, drop, st);
	auto order_keys = engine.OrderKeys(after1, drop);
	engine.Style(after1, attrs, drop);
	auto frontmatter = engine.Frontmatter(after1);
	bool any_order = std::find_if(order_keys.begin(), order_keys.end(), [](int64_t k) { return k != 0; }) !=
	                 order_keys.end();

	auto &items = ListValue::GetChildren(list);
	bool any_drop = std::find(drop.begin(), drop.end(), true) != drop.end();
	bool any_attr = false;
	for (idx_t i = 0; i < n_rows; i++) {
		any_attr = any_attr || attrs[i] != tree.nodes[i].attributes;
	}
	if (!any_drop && !any_attr && !st.changed && !ct.changed && !any_order && frontmatter.empty()) {
		engine.result.blocks = items; // nothing to do: the input, unchanged
		return std::move(engine.result);
	}

	auto &attr_type = StructType::GetChildType(block_type, fields.attributes);
	auto make_map = [&](const std::map<string, string> &m) {
		vector<Value> keys, values;
		for (auto &kv : m) {
			keys.emplace_back(kv.first);
			values.emplace_back(kv.second);
		}
		return Value::MAP(MapType::KeyType(attr_type), MapType::ValueType(attr_type), std::move(keys),
		                  std::move(values));
	};
	// text-transform and -dcs-replace are inherited: the nearest block that sets them wins
	vector<int64_t> tt_from(n_rows, -1), rp_from(n_rows, -1);
	for (idx_t i = 0; i < n_rows; i++) {
		auto p = tree.nodes[i].parent;
		tt_from[i] = ct.has_transform[i] ? static_cast<int64_t>(i) : (p >= 0 ? tt_from[p] : -1);
		rp_from[i] = ct.has_replace[i] ? static_cast<int64_t>(i) : (p >= 0 ? rp_from[p] : -1);
	}
	std::unordered_map<string, std::unique_ptr<duckdb_re2::RE2>> regexes;
	auto transform_text = [&](string text, idx_t owner, const string &type, const string &encoding) {
		if (type == "raw" || encoding == "json") {
			return text; // markup and data are not prose
		}
		if (rp_from[owner] >= 0) {
			auto &pattern = ct.replace_re[rp_from[owner]];
			auto it = regexes.find(pattern);
			if (it == regexes.end()) {
				it = regexes.emplace(pattern, std::unique_ptr<duckdb_re2::RE2>(new duckdb_re2::RE2(pattern, duckdb_re2::RE2::Quiet))).first;
			}
			if (it->second->ok()) {
				duckdb_re2::RE2::GlobalReplace(&text, *it->second, ct.replace_with[rp_from[owner]]);
			}
		}
		if (tt_from[owner] >= 0) {
			text = TextTransform(text, ct.transform[tt_from[owner]]);
		}
		return text;
	};

	int32_t next_order = 0;
	vector<int64_t> out_key; // phase 5's sort key, one per emitted row
	auto emit_gen = [&](const Gen &g, idx_t anchor, int32_t level, int64_t key) {
		out_key.push_back(key);
		auto children = StructValue::GetChildren(items[anchor]); // copies any extra fields (filename)
		children[fields.kind] = Value(g.kind);
		children[fields.element_type] = Value(g.element_type);
		children[fields.content] = Value(transform_text(g.content, anchor, g.element_type, "text"));
		children[fields.level] = Value::INTEGER(level);
		children[fields.encoding] = Value("text");
		children[fields.attributes] = make_map(g.attrs);
		children[fields.element_order] = Value::INTEGER(next_order++);
		engine.result.blocks.push_back(Value::STRUCT(block_type, std::move(children)));
	};
	auto new_level = [&](idx_t i) { return tree.nodes[i].level + st.delta[i]; };
	vector<idx_t> open; // blocks whose subtree is being emitted
	auto close_until = [&](idx_t row) {
		while (!open.empty() && tree.nodes[open.back()].subtree_end <= row) {
			auto c = open.back();
			open.pop_back();
			for (auto &g : ct.last_children[c]) {
				emit_gen(g, c, new_level(c) + 1, 0);
			}
			for (auto &g : ct.after_siblings[c]) {
				emit_gen(g, c, new_level(c), order_keys[c]);
			}
		}
	};
	idx_t skip_until = 0; // inline descendants of a block whose content was replaced
	for (idx_t i = 0; i < n_rows; i++) {
		close_until(i);
		if (drop[i] || i < skip_until || items[i].IsNull()) {
			if (items[i].IsNull() && !drop[i] && i >= skip_until) {
				engine.result.blocks.push_back(items[i]);
				out_key.push_back(0);
				next_order++;
			}
			continue;
		}
		for (auto &ins : st.before[i]) {
			Gen wrapper;
			wrapper.kind = "block";
			wrapper.element_type = ins.element_type;
			if (!ins.cls.empty()) {
				wrapper.attrs["class"] = ins.cls;
			}
			emit_gen(wrapper, i, ins.level, order_keys[i]);
		}
		for (auto &g : ct.before_siblings[i]) {
			emit_gen(g, i, new_level(i), order_keys[i]);
		}
		if (!st.unwrapped[i]) {
			auto &node = tree.nodes[i];
			auto children = StructValue::GetChildren(items[i]);
			auto type = st.element_type[i].empty() ? node.element_type : st.element_type[i];
			auto content = ct.replaced[i] ? ct.new_content[i] : node.content;
			children[fields.element_order] = Value::INTEGER(next_order++);
			children[fields.level] = Value::INTEGER(new_level(i));
			children[fields.element_type] = Value(type);
			auto owner = i; // tt_from and rp_from already inherit along the parent chain
			if (!fields_content_null(items[i], fields) || ct.replaced[i]) {
				children[fields.content] = Value(transform_text(content, owner, type, node.encoding));
			}
			if (attrs[i] != node.attributes) {
				children[fields.attributes] = make_map(attrs[i]);
			}
			engine.result.blocks.push_back(Value::STRUCT(block_type, std::move(children)));
			out_key.push_back(order_keys[i]);
		}
		for (auto &g : ct.first_children[i]) {
			emit_gen(g, i, new_level(i) + 1, 0);
		}
		for (auto &g : ct.materialized[i]) {
			emit_gen(g, i, new_level(i) + 1, 0);
		}
		if (ct.replaced[i]) {
			skip_until = tree.nodes[i].subtree_end; // a replaced block has no inline children left
		}
		if (!st.unwrapped[i]) {
			open.push_back(i);
		}
	}
	close_until(n_rows);
	auto &out = engine.result.blocks;
	// A row's level; a NULL row has none, so it takes the previous row's (1 at the start) and stays
	// beside it. Never calls GetChildren on a NULL Value.
	auto levels_of = [&]() {
		vector<int32_t> lv(out.size(), 1);
		for (idx_t k = 0; k < out.size(); k++) {
			if (out[k].IsNull()) {
				lv[k] = k > 0 ? lv[k - 1] : 1;
				continue;
			}
			auto &v = StructValue::GetChildren(out[k])[fields.level];
			lv[k] = v.IsNull() ? (k > 0 ? lv[k - 1] : 1) : v.GetValue<int32_t>();
		}
		return lv;
	};

	// phase 5: stable-sort each set of siblings by key; a block moves with its subtree. One pass to
	// find parents, one iterative walk: no recursion, and O(n log n) however deep the document is.
	if (any_order) {
		auto lv = levels_of();
		vector<vector<idx_t>> children(out.size());
		vector<idx_t> roots, stack;
		for (idx_t k = 0; k < out.size(); k++) {
			while (!stack.empty() && lv[stack.back()] >= lv[k]) {
				stack.pop_back();
			}
			(stack.empty() ? roots : children[stack.back()]).push_back(k);
			stack.push_back(k);
		}
		auto by_key = [&](idx_t x, idx_t y) { return out_key[x] < out_key[y]; };
		std::stable_sort(roots.begin(), roots.end(), by_key);
		for (auto &c : children) {
			std::stable_sort(c.begin(), c.end(), by_key);
		}
		vector<idx_t> perm, todo(roots.rbegin(), roots.rend());
		perm.reserve(out.size());
		while (!todo.empty()) {
			auto k = todo.back();
			todo.pop_back();
			perm.push_back(k);
			todo.insert(todo.end(), children[k].rbegin(), children[k].rend());
		}
		vector<Value> sorted;
		sorted.reserve(out.size());
		for (auto k : perm) {
			sorted.push_back(std::move(out[k]));
		}
		out = std::move(sorted);
	}

	// phase 6: @frontmatter, merged into an existing front matter block or a new one at the start
	if (!frontmatter.empty()) {
		int64_t existing = -1;
		for (idx_t k = 0; k < out.size() && existing < 0; k++) {
			if (out[k].IsNull()) {
				continue;
			}
			auto &f = StructValue::GetChildren(out[k]);
			if (!f[fields.element_type].IsNull() && f[fields.element_type].ToString() == "metadata") {
				auto &attr_map = f[fields.attributes];
				if (!attr_map.IsNull()) {
					for (auto &kv : MapValue::GetChildren(attr_map)) {
						auto &pair = StructValue::GetChildren(kv);
						if (!pair[0].IsNull() && pair[0].ToString() == "role" && !pair[1].IsNull() &&
						    pair[1].ToString() == "frontmatter") {
							existing = static_cast<int64_t>(k);
						}
					}
				}
			}
		}
		if (existing >= 0) {
			// replace the lines for keys the sheet sets (a line starting "key:"), append the rest
			auto children = StructValue::GetChildren(out[existing]);
			string old_text = children[fields.content].IsNull() ? string() : children[fields.content].ToString();
			std::istringstream in(old_text);
			string line, text;
			std::set<string> written;
			while (std::getline(in, line)) {
				for (auto &kv : frontmatter) {
					if (line.compare(0, kv.first.size() + 1, kv.first + ":") == 0) {
						line = kv.first + ": " + kv.second;
						written.insert(kv.first);
					}
				}
				text += line + "\n";
			}
			for (auto &kv : frontmatter) {
				if (!written.count(kv.first)) {
					text += kv.first + ": " + kv.second + "\n";
				}
			}
			children[fields.content] = Value(text);
			out[existing] = Value::STRUCT(block_type, std::move(children));
		} else {
			string text;
			for (auto &kv : frontmatter) {
				text += kv.first + ": " + kv.second + "\n";
			}
			auto lv = levels_of();
			idx_t at = 0;
			while (at < out.size() && !out[at].IsNull() && lv[at] == 0) {
				at++; // after a level-0 document root
			}
			vector<Value> fields_v;
			if (!out.empty() && !out[0].IsNull()) {
				fields_v = StructValue::GetChildren(out[0]); // copies extra fields (filename)
			} else {
				for (idx_t f = 0; f < StructType::GetChildCount(block_type); f++) {
					fields_v.emplace_back(StructType::GetChildType(block_type, f));
				}
			}
			fields_v[fields.kind] = Value("block");
			fields_v[fields.element_type] = Value("metadata");
			fields_v[fields.content] = Value(text);
			fields_v[fields.level] = Value::INTEGER(1);
			fields_v[fields.encoding] = Value("yaml");
			std::map<string, string> role {{"role", "frontmatter"}};
			fields_v[fields.attributes] = make_map(role);
			out.insert(out.begin() + static_cast<int64_t>(at), Value::STRUCT(block_type, std::move(fields_v)));
		}
	}

	// element_order follows the final row order
	if (any_order || !frontmatter.empty()) {
		for (idx_t k = 0; k < out.size(); k++) {
			if (out[k].IsNull()) {
				continue;
			}
			auto children = StructValue::GetChildren(out[k]);
			children[fields.element_order] = Value::INTEGER(static_cast<int32_t>(k));
			out[k] = Value::STRUCT(block_type, std::move(children));
		}
	}
	return std::move(engine.result);
}


} // namespace dmb
} // namespace duckdb
