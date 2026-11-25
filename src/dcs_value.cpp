#include "dcs_value.hpp"

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace dmb {

namespace {

LogicalType DeclarationType() {
	child_list_t<LogicalType> fields {
	    {"property", LogicalType::VARCHAR}, {"value", LogicalType::VARCHAR}, {"extra", LogicalType::JSON()}};
	return LogicalType::STRUCT(std::move(fields));
}

LogicalType EntryType() {
	child_list_t<LogicalType> fields {{"selector", LogicalType::VARCHAR},
	                                  {"meta", LogicalType::VARCHAR},
	                                  {"declarations", LogicalType::LIST(DeclarationType())},
	                                  {"extra", LogicalType::JSON()}};
	return LogicalType::STRUCT(std::move(fields));
}

void JsonString(string &out, const string &s) {
	out += '"';
	for (unsigned char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out += static_cast<char>(c);
			}
		}
	}
	out += '"';
}

Value JsonOrNull(const string &text) {
	return text.empty() ? Value(LogicalType::JSON()) : Value(text).DefaultCastAs(LogicalType::JSON());
}

Value OptString(const string &s) {
	return s.empty() ? Value(LogicalType::VARCHAR) : Value(s);
}

Value PosValue(int32_t v) {
	// Text positions are 1-based; 0 means "no position", and is left out.
	return v > 0 ? Value::INTEGER(v) : Value(LogicalType::INTEGER);
}

Value DeclarationValue(const Declaration &d) {
	auto extra = WriteExtra({{"important", d.important ? Value::BOOLEAN(true) : Value(LogicalType::BOOLEAN)},
	                         {"line", PosValue(d.pos.line)},
	                         {"column", PosValue(d.pos.column)}});
	return Value::STRUCT(DeclarationType(), {Value(d.property), Value(d.value_text), JsonOrNull(extra)});
}

Value DeclarationList(const vector<Declaration> &decls) {
	vector<Value> values;
	for (auto &d : decls) {
		values.push_back(DeclarationValue(d));
	}
	return Value::LIST(DeclarationType(), std::move(values));
}

} // namespace

LogicalType DcsType() {
	return LogicalType::LIST(EntryType());
}

string WriteExtra(const vector<std::pair<string, Value>> &keys) {
	string out;
	for (auto &kv : keys) {
		if (kv.second.IsNull()) {
			continue;
		}
		out += out.empty() ? "{" : ",";
		JsonString(out, kv.first);
		out += ':';
		switch (kv.second.type().id()) {
		case LogicalTypeId::BOOLEAN:
			out += kv.second.GetValue<bool>() ? "true" : "false";
			break;
		case LogicalTypeId::INTEGER:
		case LogicalTypeId::BIGINT:
			out += kv.second.ToString();
			break;
		default:
			JsonString(out, kv.second.ToString());
		}
	}
	return out.empty() ? out : out + "}";
}

Value SheetToDcs(const Sheet &sheet) {
	vector<Value> entries;
	for (auto &rule : sheet.rules) {
		auto extra = WriteExtra({{"pseudo_element", OptString(rule.pseudo_element)},
		                         {"media", OptString(rule.media_format)},
		                         {"line", PosValue(rule.pos.line)},
		                         {"column", PosValue(rule.pos.column)}});
		entries.push_back(Value::STRUCT(EntryType(), {Value(rule.selector_text), Value(LogicalType::VARCHAR),
		                                              DeclarationList(rule.declarations), JsonOrNull(extra)}));
	}
	if (!sheet.frontmatter.empty()) {
		entries.push_back(Value::STRUCT(EntryType(), {Value(LogicalType::VARCHAR), Value("frontmatter"),
		                                              DeclarationList(sheet.frontmatter), JsonOrNull("")}));
	}
	return Value::LIST(EntryType(), std::move(entries));
}

void RegisterDcsType(ExtensionLoader &loader) {
	loader.RegisterType("DCS", DcsType());
}

} // namespace dmb
} // namespace duckdb
