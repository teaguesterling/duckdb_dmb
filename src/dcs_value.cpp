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

// ---------------------------------------------------------------------------
// From a DCS value to a sheet
// ---------------------------------------------------------------------------

namespace {

//! A small JSON reader: enough for `extra`, which dmb writes as a flat object.
struct JsonReader {
	const string &s;
	size_t i = 0;
	string error;

	explicit JsonReader(const string &s_p) : s(s_p) {
	}
	void Space() {
		while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
			i++;
		}
	}
	bool Fail(const string &what) {
		if (error.empty()) {
			error = what + " at byte " + std::to_string(i);
		}
		return false;
	}
	bool Hex4(size_t at, uint32_t &code) {
		if (at + 4 > s.size()) {
			return false;
		}
		code = 0;
		for (size_t k = at; k < at + 4; k++) {
			char h = s[k];
			code <<= 4;
			if (h >= '0' && h <= '9') {
				code |= h - '0';
			} else if (h >= 'a' && h <= 'f') {
				code |= h - 'a' + 10;
			} else if (h >= 'A' && h <= 'F') {
				code |= h - 'A' + 10;
			} else {
				return false;
			}
		}
		return true;
	}
	bool String(string &out) {
		if (i >= s.size() || s[i] != '"') {
			return Fail("expected a string");
		}
		for (i++; i < s.size(); i++) {
			char c = s[i];
			if (c == '"') {
				i++;
				return true;
			}
			if (c != '\\') {
				out += c;
				continue;
			}
			if (++i >= s.size()) {
				break;
			}
			switch (s[i]) {
			case 'n':
				out += '\n';
				break;
			case 't':
				out += '\t';
				break;
			case 'r':
				out += '\r';
				break;
			case 'b':
				out += '\b';
				break;
			case 'f':
				out += '\f';
				break;
			case 'u': {
				uint32_t code;
				if (!Hex4(i + 1, code)) {
					return Fail("a \\u escape needs four hex digits");
				}
				i += 4;
				if (code >= 0xD800 && code < 0xDC00) { // a surrogate pair: \uD83D\uDE00
					uint32_t low;
					if (i + 2 >= s.size() || s[i + 1] != '\\' || s[i + 2] != 'u' || !Hex4(i + 3, low) || low < 0xDC00 ||
					    low >= 0xE000) {
						return Fail("a lone surrogate in a \\u escape");
					}
					i += 6;
					code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
				} else if (code >= 0xDC00 && code < 0xE000) {
					return Fail("a lone surrogate in a \\u escape");
				}
				if (code >= 0x10000) {
					out += static_cast<char>(0xF0 | (code >> 18));
					out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
					out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
					out += static_cast<char>(0x80 | (code & 0x3F));
				} else if (code < 0x80) {
					out += static_cast<char>(code);
				} else if (code < 0x800) {
					out += static_cast<char>(0xC0 | (code >> 6));
					out += static_cast<char>(0x80 | (code & 0x3F));
				} else {
					out += static_cast<char>(0xE0 | (code >> 12));
					out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
					out += static_cast<char>(0x80 | (code & 0x3F));
				}
				break;
			}
			default:
				out += s[i];
			}
		}
		return Fail("an unterminated string");
	}
	//! One value. Scalars go to `out`; a nested object or array is skipped (out stays NULL).
	bool Element(Value &out, int depth) {
		if (depth > MAX_NESTING) {
			return Fail("nested too deep");
		}
		Space();
		if (i >= s.size()) {
			return Fail("expected a value");
		}
		char c = s[i];
		if (c == '"') {
			string str;
			if (!String(str)) {
				return false;
			}
			out = Value(str);
			return true;
		}
		if (c == '{' || c == '[') {
			char close = c == '{' ? '}' : ']';
			i++;
			Space();
			if (i < s.size() && s[i] == close) {
				i++;
				return true;
			}
			while (true) {
				Space();
				if (c == '{') {
					string key;
					if (!String(key)) {
						return false;
					}
					Space();
					if (i >= s.size() || s[i] != ':') {
						return Fail("expected ':'");
					}
					i++;
				}
				Value ignored;
				if (!Element(ignored, depth + 1)) {
					return false;
				}
				Space();
				if (i < s.size() && s[i] == ',') {
					i++;
					continue;
				}
				if (i < s.size() && s[i] == close) {
					i++;
					return true;
				}
				return Fail(string("expected ',' or '") + close + "'");
			}
		}
		for (auto word : {"true", "false", "null"}) {
			auto len = strlen(word);
			if (s.compare(i, len, word) == 0) {
				i += len;
				out = word[0] == 'n' ? Value(LogicalType::VARCHAR) : Value::BOOLEAN(word[0] == 't');
				return true;
			}
		}
		auto start = i;
		if (i < s.size() && s[i] == '-') {
			i++;
		}
		bool integer = true;
		while (i < s.size() && (isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' || s[i] == 'e' ||
		                        s[i] == 'E' || s[i] == '+' || s[i] == '-')) {
			integer = integer && isdigit(static_cast<unsigned char>(s[i]));
			i++;
		}
		auto text = s.substr(start, i - start);
		if (text.empty() || text == "-") {
			return Fail("expected a value");
		}
		if (integer && text.size() < 18) {
			out = Value::BIGINT(std::stoll(text));
		} else {
			out = Value::DOUBLE(std::strtod(text.c_str(), nullptr));
		}
		return true;
	}
};

const Value *Child(const Value &v, idx_t i) {
	return &StructValue::GetChildren(v)[i];
}

string Str(const Value &v) {
	return v.IsNull() ? string() : v.ToString();
}

//! Reads `extra` for one entry or declaration. A bad extra is one bad-extra diagnostic, and reads as empty.
std::map<string, Value> Extra(const Value &extra, const string &what, vector<Diagnostic> &diagnostics) {
	std::map<string, Value> keys;
	string error;
	if (!ReadExtra(extra, keys, error)) {
		diagnostics.push_back(Diagnostic {"error", "bad-extra", what + ": extra is not a JSON object: " + error, {}});
		keys.clear();
	}
	return keys;
}

//! A known key of the wrong JSON type is a bad-extra diagnostic; the key then reads as absent.
bool Known(std::map<string, Value> &keys, const string &key, LogicalTypeId type, const string &what,
           vector<Diagnostic> &diagnostics) {
	auto it = keys.find(key);
	if (it == keys.end() || it->second.IsNull()) {
		return false;
	}
	if (it->second.type().id() != type) {
		diagnostics.push_back(Diagnostic {"error", "bad-extra",
		                                  what + ": extra." + key + " must be a JSON " +
		                                      (type == LogicalTypeId::VARCHAR ? "string"
		                                       : type == LogicalTypeId::BOOLEAN ? "boolean"
		                                                                        : "integer"),
		                                  {}});
		keys.erase(it);
		return false;
	}
	return true;
}

SourcePos ExtraPos(std::map<string, Value> &keys, const string &what, vector<Diagnostic> &diagnostics) {
	SourcePos pos;
	if (Known(keys, "line", LogicalTypeId::BIGINT, what, diagnostics) &&
	    Known(keys, "column", LogicalTypeId::BIGINT, what, diagnostics)) {
		pos.line = static_cast<int32_t>(keys["line"].GetValue<int64_t>());
		pos.column = static_cast<int32_t>(keys["column"].GetValue<int64_t>());
	} else {
		Known(keys, "column", LogicalTypeId::BIGINT, what, diagnostics);
	}
	return pos;
}

//! What text cannot hold in a value: an unterminated string, a comment, a ';', '{' or '}' outside a
//! string, or a trailing !important. Every other value check is left to CheckSheet and Apply, as for text:
//! the parser does not tokenize values, so the converter must not either (the spec's first anti-goal).
string ValueProblem(const string &value) {
	for (size_t i = 0; i < value.size(); i++) {
		char c = value[i];
		if (c == '"' || c == '\'') {
			size_t j = i + 1;
			while (j < value.size() && value[j] != c && value[j] != '\n') {
				j += value[j] == '\\' ? 2 : 1;
			}
			if (j >= value.size() || value[j] != c) {
				return "an unterminated string";
			}
			i = j;
		} else if (c == '/' && i + 1 < value.size() && value[i + 1] == '*') {
			return "a value can't hold a comment";
		} else if (c == ';' || c == '}' || c == '{') {
			return string("a value can't hold '") + c + "' outside a string";
		}
	}
	auto end = value.find_last_not_of(" \t\r\n");
	auto bang = value.rfind("!important");
	if (bang != string::npos && end != string::npos && bang + 10 == end + 1) {
		return "write !important in extra.important, not in the value";
	}
	return "";
}

//! A property name as text can write it: letters, digits, '-', '_', and any non-ASCII character.
bool IdentText(const string &s) {
	if (s.empty()) {
		return false;
	}
	for (unsigned char c : s) {
		if (!(c >= 0x80 || isalnum(c) || c == '-' || c == '_')) {
			return false;
		}
	}
	return true;
}

//! A media format text can write in @media (format: X): no space, bracket, ';' or ','.
bool MediaText(const string &s) {
	return !s.empty() && s.find_first_of(" \t\r\n(){};,") == string::npos;
}

vector<Declaration> Declarations(const Value &list, int32_t &order, vector<Diagnostic> &diagnostics) {
	vector<Declaration> out;
	if (list.IsNull()) {
		return out;
	}
	for (auto &d : ListValue::GetChildren(list)) {
		if (d.IsNull()) {
			continue;
		}
		auto property = Str(*Child(d, 0));
		auto value = *Child(d, 1);
		auto keys = Extra(*Child(d, 2), property.empty() ? "a declaration" : property, diagnostics);
		Declaration decl;
		decl.property = property;
		decl.pos = ExtraPos(keys, property, diagnostics);
		decl.important = Known(keys, "important", LogicalTypeId::BOOLEAN, property, diagnostics) &&
		                 keys["important"].GetValue<bool>();
		if (!IdentText(property)) {
			diagnostics.push_back(Diagnostic {
			    "error", "syntax",
			    property.empty() ? "a declaration needs a property" : "not a property name: " + property, decl.pos});
			continue;
		}
		if (value.IsNull() || value.ToString().find_first_not_of(" \t\r\n") == string::npos) {
			diagnostics.push_back(Diagnostic {"error", "empty-value", "declaration has no value: " + property, decl.pos});
			continue;
		}
		decl.value_text = value.ToString();
		auto problem = ValueProblem(decl.value_text);
		if (!problem.empty()) {
			diagnostics.push_back(Diagnostic {"error", "bad-value", property + ": " + problem, decl.pos});
			continue;
		}
		decl.order = order++;
		out.push_back(std::move(decl));
	}
	return out;
}

} // namespace

bool ReadExtra(const Value &extra, std::map<string, Value> &out, string &error) {
	out.clear();
	if (extra.IsNull()) {
		return true;
	}
	auto text = extra.ToString();
	JsonReader reader(text);
	reader.Space();
	if (reader.i >= text.size() || text[reader.i] != '{') {
		error = "expected a JSON object";
		return false;
	}
	reader.i++;
	reader.Space();
	if (reader.i < text.size() && text[reader.i] == '}') {
		reader.i++;
	} else {
		while (true) {
			reader.Space();
			string key;
			if (!reader.String(key)) {
				error = reader.error;
				return false;
			}
			reader.Space();
			if (reader.i >= text.size() || text[reader.i] != ':') {
				reader.Fail("expected ':'");
				error = reader.error;
				return false;
			}
			reader.i++;
			Value v(LogicalType::VARCHAR);
			if (!reader.Element(v, 1)) {
				error = reader.error;
				return false;
			}
			out[key] = v;
			reader.Space();
			if (reader.i < text.size() && text[reader.i] == ',') {
				reader.i++;
				continue;
			}
			if (reader.i < text.size() && text[reader.i] == '}') {
				reader.i++;
				break;
			}
			reader.Fail("expected ',' or '}'");
			error = reader.error;
			return false;
		}
	}
	reader.Space();
	if (reader.i != text.size()) {
		error = "text after the JSON object at byte " + std::to_string(reader.i);
		return false;
	}
	return true;
}

Sheet DcsToSheet(const Value &dcs) {
	Sheet sheet;
	if (dcs.IsNull()) {
		return sheet;
	}
	auto &diags = sheet.diagnostics;
	int32_t order = 0;
	int32_t rule_index = 0;
	bool have_frontmatter = false;
	// a run of entries with equal declarations is one split selector list: they share orders, as in text
	const Value *previous_declarations = nullptr;
	vector<Declaration> previous;
	for (auto &entry : ListValue::GetChildren(dcs)) {
		if (entry.IsNull()) {
			continue;
		}
		auto &selector = *Child(entry, 0);
		auto &meta = *Child(entry, 1);
		auto &declarations = *Child(entry, 2);
		auto what = selector.IsNull() ? string("a rule") : selector.ToString();
		auto keys = Extra(*Child(entry, 3), what, diags);
		auto pos = ExtraPos(keys, what, diags);
		string pseudo, media;
		if (Known(keys, "pseudo_element", LogicalTypeId::VARCHAR, what, diags)) {
			pseudo = keys["pseudo_element"].ToString();
		}
		if (Known(keys, "media", LogicalTypeId::VARCHAR, what, diags)) {
			media = keys["media"].ToString();
		}
		vector<Declaration> decls;
		if (previous_declarations && Value::NotDistinctFrom(*previous_declarations, declarations)) {
			decls = previous;
		} else {
			decls = Declarations(declarations, order, diags);
		}
		previous_declarations = &declarations;
		previous = decls;

		if (!meta.IsNull()) {
			auto m = meta.ToString();
			if (m != "frontmatter") {
				diags.push_back(Diagnostic {"error", "syntax", "unknown meta '" + m + "'", pos});
			} else if (have_frontmatter) {
				diags.push_back(Diagnostic {"error", "syntax", "a sheet has one @frontmatter entry", pos});
			} else if (!media.empty()) {
				diags.push_back(Diagnostic {"error", "syntax", "@frontmatter can't have a media format", pos});
			} else {
				have_frontmatter = true;
				sheet.frontmatter = std::move(decls);
			}
			continue;
		}
		Rule rule;
		rule.index = rule_index++;
		rule.pos = pos;
		rule.pseudo_element = pseudo;
		rule.declarations = std::move(decls);
		if (!media.empty() && !MediaText(media)) {
			diags.push_back(Diagnostic {"error", "syntax", "not a media format: " + media, pos});
			media.clear();
			rule.usable = false;
		}
		rule.media_format = media;
		if (selector.IsNull()) {
			diags.push_back(Diagnostic {"error", "syntax", "a rule needs a selector", pos});
			rule.usable = false;
		} else {
			rule.selector_text = selector.ToString();
			auto text = rule.selector_text + (pseudo.empty() ? "" : "::" + pseudo);
			string compiled_pseudo;
			if (rule.usable) {
				rule.usable = CompileSelectorAt(text, rule.selector, compiled_pseudo, diags, pos);
			}
			rule.pseudo_element = compiled_pseudo;
		}
		sheet.rules.push_back(std::move(rule));
	}
	return sheet;
}

Sheet SheetArgument(const Value &arg) {
	if (arg.type().id() == LogicalTypeId::LIST) {
		return DcsToSheet(arg.type() == DcsType() ? arg : arg.DefaultCastAs(DcsType()));
	}
	return ParseSheet(arg.ToString());
}

void ResolveSheetArgument(ResolveScalarFunctionTypesInput &input, idx_t index) {
	auto &type = input.GetArgumentType(index);
	if (type.id() == LogicalTypeId::LIST) {
		input.GetBoundFunction().GetArguments()[index] = DcsType();
	} else if (type.id() != LogicalTypeId::SQLNULL && type.id() != LogicalTypeId::UNKNOWN) {
		input.GetBoundFunction().GetArguments()[index] = LogicalType::VARCHAR;
	}
}

string FormatDcs(const Value &dcs) {
	string out;
	string open_media;
	auto close_media = [&]() {
		if (!open_media.empty()) {
			out += "}\n";
			open_media.clear();
		}
	};
	auto keys_of = [](const Value &extra) {
		std::map<string, Value> keys;
		string error;
		if (!ReadExtra(extra, keys, error)) {
			keys.clear();
		}
		return keys;
	};
	auto text_of = [](std::map<string, Value> &keys, const string &key) {
		auto it = keys.find(key);
		return it == keys.end() || it->second.IsNull() ? string() : it->second.ToString();
	};
	for (auto &entry : ListValue::GetChildren(dcs)) {
		if (entry.IsNull()) {
			continue;
		}
		auto keys = keys_of(*Child(entry, 3));
		auto media = text_of(keys, "media");
		auto &meta = *Child(entry, 1);
		bool frontmatter = !meta.IsNull() && meta.ToString() == "frontmatter";
		if (frontmatter || media != open_media) {
			close_media();
		}
		if (!frontmatter && !media.empty() && open_media.empty()) {
			out += "@media (format: " + media + ") {\n";
			open_media = media;
		}
		if (frontmatter) {
			out += "@frontmatter";
		} else {
			out += Str(*Child(entry, 0));
			auto pseudo = text_of(keys, "pseudo_element");
			if (!pseudo.empty()) {
				out += "::" + pseudo;
			}
		}
		out += " {";
		auto &declarations = *Child(entry, 2);
		if (!declarations.IsNull()) {
			for (auto &d : ListValue::GetChildren(declarations)) {
				if (d.IsNull()) {
					continue;
				}
				auto dkeys = keys_of(*Child(d, 2));
				auto important = dkeys.find("important");
				bool is_important = important != dkeys.end() && important->second.type().id() == LogicalTypeId::BOOLEAN &&
				                    important->second.GetValue<bool>();
				out += " " + Str(*Child(d, 0)) + ": " + Str(*Child(d, 1)) + (is_important ? " !important" : "") + ";";
			}
		}
		out += " }\n";
	}
	close_media();
	return out;
}

void RegisterDcsType(ExtensionLoader &loader) {
	loader.RegisterType("DCS", DcsType());
}

} // namespace dmb
} // namespace duckdb
