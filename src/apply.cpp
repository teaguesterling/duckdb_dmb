#include "apply.hpp"
#include "matcher.hpp"
#include "values.hpp"

#include "duckdb/common/error_data.hpp"

#include <algorithm>
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
	static const std::set<string> s {"content",         "-dcs-content-as",
	                                 "-dcs-content-place", "text-transform",   "text-overflow",   "-dcs-replace",
	                                 "-dcs-link",        "counter-reset",      "counter-increment", "order"};
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

bool IsCustom(const string &p) {
	return p.size() > 2 && p[0] == '-' && p[1] == '-';
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

	void Diag(const string &severity, const string &code, const string &message, SourcePos pos) {
		result.diagnostics.push_back(Diagnostic {severity, code, message, pos});
	}

	//! Per node, per property: the winning declaration among the rules that pass `want`.
	template <class WANT>
	vector<std::map<string, Winner>> Cascade(const BlockTree &tree, WANT want) {
		vector<std::map<string, Winner>> out(tree.nodes.size());
		Matcher matcher(tree);
		for (auto &rule : sheet.rules) {
			if (!rule.usable || !rule.media_format.empty() || !rule.pseudo_element.empty()) {
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
				auto &p = d.property;
				if (IsCustom(p)) {
					if (ReservedAttributes().count(p.substr(2))) {
						Diag("error", "reserved-attribute",
						     p + " is reserved: set " + p.substr(2) + " through its own property", d.pos);
					}
				} else if (IsPhase1(p)) {
					continue;
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
				} else if (LaterProperties().count(p)) {
					Diag("warning", "not-yet", p + " is not supported yet", d.pos);
				} else if (p.compare(0, 5, "-dcs-") == 0) {
					Diag("error", "unknown-property", "unknown property " + p, d.pos);
				} else {
					Diag("warning", "not-yet", "presentation property " + p + " is not supported yet", d.pos);
				}
				if (!rule.pseudo_element.empty() && !LaterProperties().count(p)) {
					Diag("warning", "not-yet", "::" + rule.pseudo_element + " is not supported yet", d.pos);
				}
			}
		}
	}

	bool Evaluate(const ValueEvaluator &eval, const Declaration &d, idx_t node, string &out, bool &is_null) {
		vector<Token> tokens;
		string error;
		if (!TokenizeValue(d.value_text, tokens, error) || !eval.Eval(tokens, node, out, is_null, error)) {
			auto code = error.compare(0, 16, "unknown-function") == 0 ? "unknown-function" : "bad-value";
			Diag("error", code, d.property + ": " + error, d.pos);
			return false;
		}
		return true;
	}

	void Record(const BlockTree &tree, idx_t i, const Winner &w, const Value &before, const Value &after) {
		if (before.IsNull() || after.IsNull() ? before.IsNull() == after.IsNull()
		                                       : before.ToString() == after.ToString()) {
			return; // unchanged (Value == refuses NULL on v2.0)
		}
		result.changes.push_back(Change {tree.nodes[i].element_order, w.rule->index, w.decl->property, before, after});
	}

	void Phase1(const BlockTree &tree, vector<std::map<string, string>> &attrs) {
		auto cascade = Cascade(tree, IsPhase1);
		ValueEvaluator eval(tree);
		std::set<string> ids;
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
							for (int n = 1; ids.count(id); n++) {
								id = base + "-" + std::to_string(n);
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
							for (int n = 1; ids.count(id) && !(own != a.end() && own->second == id); n++) {
								id = v + "-" + std::to_string(n);
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
			                                 Value("present"), Value(LogicalType::VARCHAR)});
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
			                                 Value(before), Value(after)});
			st.changed = true;
		};
		for (idx_t i = 0; i < n_rows; i++) {
			if (drop[i] || !tree.Matchable(i)) {
				continue;
			}
			auto &node = tree.nodes[i];
			auto &props = cascade[i];
			// display: contents
			auto dw = display[i].find("display");
			if (dw != display[i].end() && Trim(dw->second.decl->value_text) == "contents") {
				bool inline_child = false;
				for (idx_t j = i + 1; j < node.subtree_end; j++) {
					if (tree.nodes[j].parent == static_cast<int64_t>(i) && tree.nodes[j].kind == "inline") {
						inline_child = true;
					}
				}
				if (inline_child) {
					Diag("error", "contents-has-inlines",
					     "display: contents needs a container of blocks; this block holds inline text",
					     dw->second.decl->pos);
				} else {
					st.unwrapped[i] = true;
					for (idx_t j = i + 1; j < node.subtree_end; j++) {
						st.delta[j]--;
					}
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
						a["dcs-as"] = "table"; // D7: dcs_apply_parsed converts it
						record(i, as->second, type, "table (marked)");
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
					st.before[i].push_back(Inserted {wt, wc, node.level + st.delta[i]});
					for (idx_t j = i; j < node.subtree_end; j++) {
						st.delta[j]++;
					}
					record(i, ww->second, "", wt + (wc.empty() ? "" : "." + wc));
				}
			}
		}
		return st;
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

ApplyResult Apply(const Value &list, const BlockFields &fields, const LogicalType &block_type, const Sheet &sheet) {
	Engine engine(sheet);
	engine.CheckDeclarations();
	auto tree = BlockTree::FromList(list, fields);
	vector<std::map<string, string>> attrs(tree.nodes.size());
	for (idx_t i = 0; i < tree.nodes.size(); i++) {
		attrs[i] = tree.nodes[i].attributes;
	}
	// phase 1 matches the input; everything after it matches the document after phase 1
	engine.Phase1(tree, attrs);
	auto after1 = tree;
	for (idx_t i = 0; i < after1.nodes.size(); i++) {
		after1.nodes[i].attributes = attrs[i];
	}
	engine.Attributes(after1, attrs);
	auto drop = engine.Prune(after1);
	auto st = engine.Phase3(after1, attrs, drop);
	bool renumber = st.changed || std::find(drop.begin(), drop.end(), true) != drop.end();

	auto &items = ListValue::GetChildren(list);
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
	int32_t next_order = 0;
	for (idx_t i = 0; i < items.size(); i++) {
		if (drop[i]) {
			continue;
		}
		for (auto &ins : st.before[i]) {
			if (items[i].IsNull()) {
				break;
			}
			auto children = StructValue::GetChildren(items[i]); // copies any extra fields (filename)
			children[fields.kind] = Value("block");
			children[fields.element_type] = Value(ins.element_type);
			children[fields.content] = Value("");
			children[fields.level] = Value::INTEGER(ins.level);
			std::map<string, string> wrapper_attrs;
			if (!ins.cls.empty()) {
				wrapper_attrs["class"] = ins.cls;
			}
			children[fields.attributes] = make_map(wrapper_attrs);
			children[fields.element_order] = Value::INTEGER(next_order++);
			engine.result.blocks.push_back(Value::STRUCT(block_type, std::move(children)));
		}
		if (st.unwrapped[i]) {
			continue;
		}
		int32_t order = next_order++;
		bool same_attrs = attrs[i] == tree.nodes[i].attributes;
		bool same_shape = st.delta[i] == 0 && st.element_type[i].empty();
		if (items[i].IsNull() || (same_attrs && same_shape && !renumber)) {
			engine.result.blocks.push_back(items[i]);
			continue;
		}
		auto children = StructValue::GetChildren(items[i]);
		children[fields.element_order] = Value::INTEGER(order);
		if (st.delta[i] != 0) {
			children[fields.level] = Value::INTEGER(tree.nodes[i].level + st.delta[i]);
		}
		if (!st.element_type[i].empty()) {
			children[fields.element_type] = Value(st.element_type[i]);
		}
		if (!same_attrs) {
			children[fields.attributes] = make_map(attrs[i]);
		}
		engine.result.blocks.push_back(Value::STRUCT(block_type, std::move(children)));
	}
	return std::move(engine.result);
}

} // namespace dmb
} // namespace duckdb
