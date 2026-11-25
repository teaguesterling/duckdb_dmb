// read_dcs_objects(globs): one row per .dcs file. read_dcs(globs): one row per rule.
// A simple glob loop, not the multi-file reader: sheets are small, and each file is read whole.

#include "readers.hpp"
#include "dcs_value.hpp"
#include "apply.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace dmb {

namespace {

struct ReaderBindData : public TableFunctionData {
	vector<vector<Value>> rows;
};

struct ReaderState : public GlobalTableFunctionState {
	idx_t offset = 0;
};

//! The files the globs match, sorted, each once. A glob that matches nothing is an error.
vector<string> Files(ClientContext &context, const Value &arg) {
	vector<string> globs;
	if (arg.type().id() == LogicalTypeId::LIST) {
		for (auto &g : ListValue::GetChildren(arg)) {
			if (!g.IsNull()) {
				globs.push_back(g.ToString());
			}
		}
	} else if (!arg.IsNull()) {
		globs.push_back(arg.ToString());
	}
	auto &fs = FileSystem::GetFileSystem(context);
	vector<string> files;
	for (auto &glob : globs) {
		for (auto &file : fs.GlobFiles(glob)) {
			files.push_back(file.path);
		}
	}
	std::sort(files.begin(), files.end());
	files.erase(std::unique(files.begin(), files.end()), files.end());
	return files;
}

string ReadWhole(ClientContext &context, const string &path) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	auto size = handle->GetFileSize();
	string content(size, '\0');
	if (size > 0) {
		handle->Read(&content[0], size, 0);
	}
	return content;
}

LogicalType DiagnosticListType() {
	child_list_t<LogicalType> fields {{"severity", LogicalType::VARCHAR},
	                                  {"code", LogicalType::VARCHAR},
	                                  {"message", LogicalType::VARCHAR},
	                                  {"line", LogicalType::INTEGER},
	                                  {"column", LogicalType::INTEGER}};
	return LogicalType::LIST(LogicalType::STRUCT(std::move(fields)));
}

Value Diagnostics(const vector<Diagnostic> &diagnostics) {
	auto type = ListType::GetChildType(DiagnosticListType());
	vector<Value> values;
	for (auto &d : diagnostics) {
		values.push_back(Value::STRUCT(type, {Value(d.severity), Value(d.code), Value(d.message),
		                                      Value::INTEGER(d.pos.line), Value::INTEGER(d.pos.column)}));
	}
	return Value::LIST(type, std::move(values));
}

unique_ptr<FunctionData> ObjectsBind(ClientContext &context, TableFunctionBindInput &input,
                                     vector<LogicalType> &types, vector<Identifier> &names) {
	names = {"filename", "content", "sheet", "diagnostics"};
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR, DcsType(), DiagnosticListType()};
	auto result = make_uniq<ReaderBindData>();
	for (auto &file : Files(context, input.inputs[0])) {
		auto content = ReadWhole(context, file);
		auto sheet = ParseSheet(content);
		result->rows.push_back({Value(file), Value(content), SheetToDcs(sheet), Diagnostics(CheckSheet(sheet))});
	}
	return std::move(result);
}

unique_ptr<FunctionData> RulesBind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &types,
                                   vector<Identifier> &names) {
	auto dcs_type = DcsType();
	auto &entry = ListType::GetChildType(dcs_type);
	names = {"filename", "rule"};
	types = {LogicalType::VARCHAR, LogicalType::INTEGER};
	for (auto &child : StructType::GetChildTypes(entry)) {
		names.push_back(Identifier(child.first));
		types.push_back(child.second);
	}
	auto result = make_uniq<ReaderBindData>();
	for (auto &file : Files(context, input.inputs[0])) {
		auto dcs = SheetToDcs(ParseSheet(ReadWhole(context, file)));
		int32_t position = 0;
		for (auto &e : ListValue::GetChildren(dcs)) {
			vector<Value> row {Value(file), Value::INTEGER(position++)};
			for (auto &field : StructValue::GetChildren(e)) {
				row.push_back(field);
			}
			result->rows.push_back(std::move(row));
		}
	}
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> ReaderInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<ReaderState>();
}

void ReaderScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &bind = data.bind_data->Cast<ReaderBindData>();
	auto &state = data.global_state->Cast<ReaderState>();
	idx_t count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, bind.rows.size() - state.offset);
	for (idx_t i = 0; i < count; i++) {
		auto &row = bind.rows[state.offset + i];
		for (idx_t c = 0; c < row.size(); c++) {
			output.data[c].SetValue(i, row[c]);
		}
	}
	state.offset += count;
	output.SetChildCardinality(count);
}

} // namespace

void RegisterReaders(ExtensionLoader &loader) {
	for (auto objects : {true, false}) {
		TableFunctionSet set(objects ? "read_dcs_objects" : "read_dcs");
		vector<LogicalType> args {LogicalType(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::VARCHAR)};
		for (auto &arg : args) {
			set.AddFunction(TableFunction(vector<LogicalType> {arg}, ReaderScan, objects ? ObjectsBind : RulesBind,
			                              ReaderInit));
		}
		loader.RegisterFunction(set);
	}
}

} // namespace dmb
} // namespace duckdb
