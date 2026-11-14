#include "matcher.hpp"

#include "re2/re2.h"

#include <algorithm>

namespace duckdb {
namespace dmb {

vector<idx_t> SectionExtent(const BlockTree &tree, idx_t h) {
	vector<idx_t> out {h};
	auto &head = tree.nodes[h];
	int32_t rank = tree.HeadingRank(h);
	for (idx_t j = h + 1; j < tree.nodes.size(); j++) {
		auto &n = tree.nodes[j];
		if (n.level < head.level) {
			break;
		}
		if (n.level == head.level && n.element_type == "heading") {
			int32_t r = tree.HeadingRank(j);
			if (r == 0 || rank == 0 || r <= rank) {
				break;
			}
		}
		out.push_back(j);
	}
	return out;
}

Matcher::Matcher(const BlockTree &tree_p) : tree(tree_p) {
}

Matcher::~Matcher() {
}

bool Matcher::Matches(const ComplexSelector &sel, idx_t node) {
	if (!tree.Matchable(node) || sel.compounds.empty()) {
		return false;
	}
	return MatchFrom(sel, sel.compounds.size() - 1, node, -1);
}

//! Does `node` stand in relation `comb` to `anchor`? (' ' descendant, '>' child, '+', '~' siblings)
bool Matcher::Related(char comb, idx_t node, idx_t anchor) const {
	auto parent = tree.nodes[node].parent;
	switch (comb) {
	case '>':
		return parent == static_cast<int64_t>(anchor);
	case '+':
	case '~': {
		auto &sibs = tree.Siblings(node);
		auto it = std::find(sibs.begin(), sibs.end(), node);
		auto at = std::find(sibs.begin(), sibs.end(), anchor);
		if (it == sibs.end() || at == sibs.end() || at >= it) {
			return false;
		}
		return comb == '~' || at + 1 == it;
	}
	default:
		for (auto p = parent; p >= 0; p = tree.nodes[p].parent) {
			if (p == static_cast<int64_t>(anchor)) {
				return true;
			}
		}
		return false;
	}
}

//! Right to left: compound k matches `node`, and compounds 0..k-1 match along the combinators.
//! `anchor` >= 0 is the :has() subject: compound 0 must stand in `sel.leading` to it.
bool Matcher::MatchFrom(const ComplexSelector &sel, idx_t k, idx_t node, int64_t anchor) {
	if (!tree.Matchable(node) || !MatchCompound(sel.compounds[k], node)) {
		return false;
	}
	if (k == 0) {
		return anchor < 0 || Related(sel.leading ? sel.leading : ' ', node, static_cast<idx_t>(anchor));
	}
	char comb = sel.combinators[k - 1];
	auto parent = tree.nodes[node].parent;
	switch (comb) {
	case '>':
		return parent >= 0 && MatchFrom(sel, k - 1, static_cast<idx_t>(parent), anchor);
	case ' ':
		for (auto p = parent; p >= 0; p = tree.nodes[p].parent) {
			if (MatchFrom(sel, k - 1, static_cast<idx_t>(p), anchor)) {
				return true;
			}
		}
		return false;
	case '+':
	case '~': {
		auto &sibs = tree.Siblings(node);
		auto it = std::find(sibs.begin(), sibs.end(), node);
		while (it != sibs.begin()) {
			--it;
			if (MatchFrom(sel, k - 1, *it, anchor)) {
				return true;
			}
			if (comb == '+') {
				return false;
			}
		}
		return false;
	}
	default:
		return false;
	}
}

bool Matcher::MatchCompound(const Compound &c, idx_t node) {
	for (auto &part : c.parts) {
		if (!MatchSimple(part, node)) {
			return false;
		}
	}
	return true;
}

static bool NthMatches(int32_t a, int32_t b, int32_t pos) {
	if (a == 0) {
		return pos == b;
	}
	int32_t diff = pos - b;
	return diff % a == 0 && diff / a >= 0;
}

duckdb_re2::RE2 &Matcher::Regex(const string &pattern) {
	auto it = regexes.find(pattern);
	if (it == regexes.end()) {
		auto re = std::unique_ptr<duckdb_re2::RE2>(new duckdb_re2::RE2(pattern));
		it = regexes.emplace(pattern, std::move(re)).first;
	}
	return *it->second;
}

bool Matcher::InSection(const SimpleSelector &s, idx_t node) {
	auto it = sections.find(&s);
	if (it == sections.end()) {
		vector<bool> in(tree.nodes.size(), false);
		for (idx_t h = 0; h < tree.nodes.size(); h++) {
			if (tree.nodes[h].element_type != "heading") {
				continue;
			}
			bool hit = false;
			for (auto &arg : s.args) {
				hit = hit || Matches(arg, h);
			}
			if (hit) {
				for (auto j : SectionExtent(tree, h)) {
					in[j] = true;
				}
			}
		}
		it = sections.emplace(&s, std::move(in)).first;
	}
	return it->second[node];
}

static string TypeAlias(const string &name) {
	if (name == "p") {
		return "paragraph";
	}
	if (name == "img") {
		return "image";
	}
	if (name == "li") {
		return "list_item";
	}
	if (name == "pre") {
		return "code";
	}
	return name;
}

bool Matcher::MatchSimple(const SimpleSelector &s, idx_t node) {
	auto &n = tree.nodes[node];
	switch (s.kind) {
	case SimpleSelector::UNIVERSAL:
		return true;
	case SimpleSelector::TYPE: {
		if (s.name.size() == 2 && s.name[0] == 'h' && s.name[1] >= '1' && s.name[1] <= '6') {
			return tree.HeadingRank(node) == s.name[1] - '0';
		}
		return n.element_type == TypeAlias(s.name);
	}
	case SimpleSelector::ID: {
		auto id = tree.Attr(node, "id");
		return id && *id == s.name;
	}
	case SimpleSelector::CLASS: {
		auto classes = tree.Classes(node);
		return std::find(classes.begin(), classes.end(), s.name) != classes.end();
	}
	case SimpleSelector::ATTR: {
		string value;
		if (s.name == "kind") {
			value = n.kind;
		} else if (s.name == "element_type") {
			value = n.element_type;
		} else if (s.name == "encoding") {
			value = n.encoding;
		} else if (s.name == "level") {
			value = std::to_string(n.level);
		} else {
			auto a = tree.Attr(node, s.name);
			if (!a) {
				return false;
			}
			value = *a;
		}
		auto &v = s.value;
		if (s.op.empty()) {
			return true;
		}
		if (s.op == "=") {
			return value == v;
		}
		if (s.op == "~=") {
			string padded = " " + value + " ";
			return !v.empty() && v.find(' ') == string::npos && padded.find(" " + v + " ") != string::npos;
		}
		if (s.op == "^=") {
			return !v.empty() && value.compare(0, v.size(), v) == 0;
		}
		if (s.op == "$=") {
			return !v.empty() && value.size() >= v.size() && value.compare(value.size() - v.size(), v.size(), v) == 0;
		}
		if (s.op == "*=") {
			return !v.empty() && value.find(v) != string::npos;
		}
		if (s.op == "|=") {
			return value == v || value.compare(0, v.size() + 1, v + "-") == 0;
		}
		return false;
	}
	case SimpleSelector::PSEUDO_CLASS:
		break;
	}
	auto &name = s.name;
	if (name == "not") {
		for (auto &arg : s.args) {
			if (Matches(arg, node)) {
				return false;
			}
		}
		return true;
	}
	if (name == "has") {
		for (auto &arg : s.args) {
			for (idx_t j = 0; j < tree.nodes.size(); j++) {
				if (j != node && !arg.compounds.empty() &&
				    MatchFrom(arg, arg.compounds.size() - 1, j, static_cast<int64_t>(node))) {
					return true;
				}
			}
		}
		return false;
	}
	if (name == "section") {
		return InSection(s, node);
	}
	if (name == "contains") {
		return tree.Text(node).find(s.text_arg) != string::npos;
	}
	if (name == "matches") {
		auto &re = Regex(s.text_arg);
		return re.ok() && duckdb_re2::RE2::PartialMatch(tree.Text(node), re);
	}
	if (name == "heading") {
		return n.element_type == "heading";
	}
	if (name == "h") {
		return tree.HeadingRank(node) == std::atoi(s.text_arg.c_str());
	}
	if (name == "empty") {
		return n.subtree_end == node + 1 && n.content.empty();
	}
	if (name == "root") {
		return n.parent < 0 || !tree.Matchable(static_cast<idx_t>(n.parent));
	}
	auto &sibs = tree.Siblings(node);
	if (name == "first-child" || name == "last-child" || name == "only-child" || name == "nth-child" ||
	    name == "nth-last-child") {
		auto pos = static_cast<int32_t>(std::find(sibs.begin(), sibs.end(), node) - sibs.begin()) + 1;
		auto count = static_cast<int32_t>(sibs.size());
		if (name == "first-child") {
			return pos == 1;
		}
		if (name == "last-child") {
			return pos == count;
		}
		if (name == "only-child") {
			return count == 1;
		}
		return NthMatches(s.a, s.b, name == "nth-child" ? pos : count - pos + 1);
	}
	if (name == "first-of-type" || name == "last-of-type" || name == "only-of-type" || name == "nth-of-type" ||
	    name == "nth-last-of-type") {
		int32_t pos = 0, count = 0;
		for (auto sib : sibs) {
			if (tree.nodes[sib].element_type == n.element_type) {
				count++;
				if (sib <= node) {
					pos = count;
				}
			}
		}
		if (name == "first-of-type") {
			return pos == 1;
		}
		if (name == "last-of-type") {
			return pos == count;
		}
		if (name == "only-of-type") {
			return count == 1;
		}
		return NthMatches(s.a, s.b, name == "nth-of-type" ? pos : count - pos + 1);
	}
	throw NotImplementedException("not-yet: dmb does not match :%s yet", name);
}

} // namespace dmb
} // namespace duckdb
