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
	static const std::set<string> s {"-dcs-wrap",
	                                 "-dcs-as",          "-dcs-level",         "content",         "-dcs-content-as",
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
				} else if (IsPhase2(p)) {
					auto v = Trim(d.value_text);
					if (p == "display" && v != "none") {
						Diag("warning", "not-yet", "display: " + v + " is not supported yet", d.pos);
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
							a["id"] = v;
							ids.insert(v);
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
				if (!seen_texts.insert(text).second) {
					mark(i, tree.nodes[i].subtree_end, i, dedupe->second);
				}
			}
		}
		return drop;
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
	bool any_dropped = std::find(drop.begin(), drop.end(), true) != drop.end();

	auto &items = ListValue::GetChildren(list);
	auto &attr_type = StructType::GetChildType(block_type, fields.attributes);
	int32_t next_order = 0;
	for (idx_t i = 0; i < items.size(); i++) {
		if (drop[i]) {
			continue;
		}
		int32_t order = next_order++;
		bool same_attrs = attrs[i] == tree.nodes[i].attributes;
		if (items[i].IsNull() || (same_attrs && !any_dropped)) {
			engine.result.blocks.push_back(items[i]);
			continue;
		}
		auto children = StructValue::GetChildren(items[i]);
		children[fields.element_order] = Value::INTEGER(order);
		if (same_attrs) {
			engine.result.blocks.push_back(Value::STRUCT(block_type, std::move(children)));
			continue;
		}
		vector<Value> keys, values;
		for (auto &kv : attrs[i]) {
			keys.emplace_back(kv.first);
			values.emplace_back(kv.second);
		}
		children[fields.attributes] =
		    Value::MAP(MapType::KeyType(attr_type), MapType::ValueType(attr_type), std::move(keys), std::move(values));
		engine.result.blocks.push_back(Value::STRUCT(block_type, std::move(children)));
	}
	return std::move(engine.result);
}

} // namespace dmb
} // namespace duckdb
