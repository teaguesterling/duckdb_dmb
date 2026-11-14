#include "block_tree.hpp"

namespace duckdb {
namespace dmb {

static const char *KIND_INLINE = "inline";
static const char *TYPE_HEADING = "heading";

BlockFields ResolveBlockFields(const LogicalType &type, const string &function_name) {
	if (type.id() != LogicalTypeId::LIST || ListType::GetChildType(type).id() != LogicalTypeId::STRUCT) {
		throw BinderException("%s: expected a duck_block[] list, got %s", function_name, type.ToString());
	}
	auto &child = ListType::GetChildType(type);
	auto find = [&](const char *name) -> idx_t {
		for (idx_t i = 0; i < StructType::GetChildCount(child); i++) {
			if (StringUtil::CIEquals(StructType::GetChildName(child, i).GetIdentifierName(), name)) {
				return i;
			}
		}
		throw BinderException("%s: the block struct has no field '%s'", function_name, name);
	};
	BlockFields f;
	f.kind = find("kind");
	f.element_type = find("element_type");
	f.content = find("content");
	f.level = find("level");
	f.encoding = find("encoding");
	f.attributes = find("attributes");
	f.element_order = find("element_order");
	return f;
}

static string Str(const Value &v) {
	return v.IsNull() ? string() : v.ToString();
}

BlockTree BlockTree::FromList(const Value &list, const BlockFields &f) {
	BlockTree tree;
	auto &items = ListValue::GetChildren(list);
	tree.nodes.resize(items.size());
	for (idx_t i = 0; i < items.size(); i++) {
		auto &n = tree.nodes[i];
		n.row = i;
		if (items[i].IsNull()) {
			n.kind = KIND_INLINE; // a NULL row is never matched
			continue;
		}
		auto &fields = StructValue::GetChildren(items[i]);
		n.kind = Str(fields[f.kind]);
		n.element_type = Str(fields[f.element_type]);
		n.content = Str(fields[f.content]);
		n.encoding = Str(fields[f.encoding]);
		n.level = fields[f.level].IsNull() ? 1 : fields[f.level].GetValue<int32_t>();
		n.element_order = fields[f.element_order].IsNull() ? static_cast<int32_t>(i)
		                                                    : fields[f.element_order].GetValue<int32_t>();
		auto &attrs = fields[f.attributes];
		if (!attrs.IsNull()) {
			for (auto &kv : MapValue::GetChildren(attrs)) {
				auto &pair = StructValue::GetChildren(kv);
				if (!pair[0].IsNull() && !pair[1].IsNull()) {
					n.attributes[pair[0].ToString()] = pair[1].ToString();
				}
			}
		}
	}
	// parents: the nearest preceding row with a smaller level
	vector<idx_t> stack;
	for (idx_t i = 0; i < tree.nodes.size(); i++) {
		auto &n = tree.nodes[i];
		while (!stack.empty() && tree.nodes[stack.back()].level >= n.level) {
			tree.nodes[stack.back()].subtree_end = i;
			stack.pop_back();
		}
		n.parent = stack.empty() ? -1 : static_cast<int64_t>(stack.back());
		stack.push_back(i);
	}
	for (auto idx : stack) {
		tree.nodes[idx].subtree_end = tree.nodes.size();
	}
	for (idx_t i = 0; i < tree.nodes.size(); i++) {
		if (!tree.Matchable(i)) {
			continue;
		}
		auto p = tree.nodes[i].parent;
		if (p >= 0 && tree.Matchable(static_cast<idx_t>(p))) {
			tree.nodes[p].children.push_back(i);
		} else {
			tree.roots.push_back(i);
		}
	}
	return tree;
}

bool BlockTree::Matchable(idx_t i) const {
	return nodes[i].kind != KIND_INLINE;
}

string BlockTree::Text(idx_t i) const {
	auto &n = nodes[i];
	if (n.element_type == TYPE_HEADING && !n.content.empty()) {
		return n.content;
	}
	string text;
	bool any_inline = false;
	for (idx_t j = i + 1; j < n.subtree_end; j++) {
		if (nodes[j].kind == KIND_INLINE) {
			text += nodes[j].content;
			any_inline = true;
		}
	}
	return any_inline ? text : n.content;
}

vector<string> BlockTree::Classes(idx_t i) const {
	vector<string> out;
	auto cls = Attr(i, "class");
	if (!cls) {
		return out;
	}
	string cur;
	for (char c : *cls) {
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
			if (!cur.empty()) {
				out.push_back(cur);
				cur.clear();
			}
		} else {
			cur += c;
		}
	}
	if (!cur.empty()) {
		out.push_back(cur);
	}
	return out;
}

const string *BlockTree::Attr(idx_t i, const string &key) const {
	auto it = nodes[i].attributes.find(key);
	return it == nodes[i].attributes.end() ? nullptr : &it->second;
}

const vector<idx_t> &BlockTree::Siblings(idx_t i) const {
	auto p = nodes[i].parent;
	if (p >= 0 && Matchable(static_cast<idx_t>(p))) {
		return nodes[p].children;
	}
	return roots;
}

int32_t BlockTree::HeadingRank(idx_t i) const {
	if (nodes[i].element_type != TYPE_HEADING) {
		return 0;
	}
	auto hl = Attr(i, "heading_level");
	if (!hl) {
		return 0;
	}
	try {
		return std::stoi(*hl);
	} catch (...) {
		return 0;
	}
}

} // namespace dmb
} // namespace duckdb
