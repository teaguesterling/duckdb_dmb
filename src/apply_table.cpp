// FROM dcs_apply((subquery), sheet [, strict]): the table form of dcs_apply.
//
// A rule such as `h2 + p` needs the whole document, so the function holds every input row and writes
// the output in its final callback. Its global state keeps the default MaxThreads() of 1, so the
// pipeline runs on one thread and one local state sees every row (measured on v2.0.1: 100000 rows
// with SET threads = 8 reached one local state and one final call).

#include "functions.hpp"
#include "apply.hpp"
#include "dcs_value.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <algorithm>
#include <map>

namespace duckdb {
namespace dmb {

namespace {

struct TableApplyBind : public TableFunctionData {
	BlockFields fields;
	LogicalType block_type;
	optional_idx filename;
	Sheet sheet;
	bool strict = false;
};

struct TableApplyGlobal : public GlobalTableFunctionState {};

struct TableApplyLocal : public LocalTableFunctionState {
	std::map<string, vector<Value>> documents; // by filename
	vector<Value> unnamed;                     // rows whose filename is NULL, or all rows when there is no column
	bool finalized = false;
	vector<Value> out;
	idx_t offset = 0;
};

unique_ptr<FunctionData> TableApplyBindFn(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &types,
                                          vector<Identifier> &names) {
	auto result = make_uniq<TableApplyBind>();
	child_list_t<LogicalType> children;
	for (idx_t i = 0; i < input.input_table_types.size(); i++) {
		auto name = input.input_table_names[i].GetIdentifierName();
		children.emplace_back(name, input.input_table_types[i]);
		if (name == "filename") {
			result->filename = i;
		}
	}
	result->block_type = LogicalType::STRUCT(children);
	result->fields = ResolveBlockFields(LogicalType::LIST(result->block_type), "dcs_apply");
	auto &sheet = input.inputs[1];
	if (sheet.IsNull()) {
		throw BinderException("dcs_apply: the sheet is NULL");
	}
	result->sheet = SheetArgument(sheet);
	result->strict = input.inputs.size() > 2 && !input.inputs[2].IsNull() && input.inputs[2].GetValue<bool>();
	types = input.input_table_types;
	names = input.input_table_names;
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> TableApplyInitGlobal(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<TableApplyGlobal>();
}

unique_ptr<LocalTableFunctionState> TableApplyInitLocal(ExecutionContext &, TableFunctionInitInput &,
                                                        GlobalTableFunctionState *) {
	return make_uniq<TableApplyLocal>();
}

OperatorResultType TableApplyIn(ExecutionContext &, TableFunctionInput &data, DataChunk &input, DataChunk &output) {
	auto &bind = data.bind_data->Cast<TableApplyBind>();
	auto &local = data.local_state->Cast<TableApplyLocal>();
	for (idx_t row = 0; row < input.size(); row++) {
		vector<Value> fields;
		for (idx_t c = 0; c < input.ColumnCount(); c++) {
			fields.push_back(input.GetValue(c, row));
		}
		auto block = Value::STRUCT(bind.block_type, std::move(fields));
		if (bind.filename.IsValid() && !input.GetValue(bind.filename.GetIndex(), row).IsNull()) {
			local.documents[input.GetValue(bind.filename.GetIndex(), row).ToString()].push_back(std::move(block));
		} else {
			local.unnamed.push_back(std::move(block));
		}
	}
	output.SetCardinality(0);
	return OperatorResultType::NEED_MORE_INPUT;
}

void ApplyDocument(const TableApplyBind &bind, vector<Value> &rows, vector<Value> &out) {
	if (rows.empty()) {
		return;
	}
	auto order = bind.fields.element_order;
	std::stable_sort(rows.begin(), rows.end(), [order](const Value &a, const Value &b) {
		auto &x = StructValue::GetChildren(a)[order];
		auto &y = StructValue::GetChildren(b)[order];
		if (x.IsNull() || y.IsNull()) {
			return !x.IsNull() && y.IsNull(); // NULL last
		}
		return x.GetValue<int64_t>() < y.GetValue<int64_t>();
	});
	auto applied = Apply(Value::LIST(bind.block_type, rows), bind.fields, bind.block_type, bind.sheet);
	if (bind.strict && !applied.diagnostics.empty()) {
		auto &d = applied.diagnostics[0];
		throw InvalidInputException("%s: %s at line %d, column %d: %s", "dcs_apply", d.code, d.pos.line, d.pos.column,
		                            d.message);
	}
	for (auto &block : applied.blocks) {
		out.push_back(std::move(block));
	}
}

OperatorFinalizeResultType TableApplyFinal(ExecutionContext &, TableFunctionInput &data, DataChunk &output) {
	auto &bind = data.bind_data->Cast<TableApplyBind>();
	auto &local = data.local_state->Cast<TableApplyLocal>();
	if (!local.finalized) {
		local.finalized = true;
		for (auto &doc : local.documents) { // std::map: filename order
			ApplyDocument(bind, doc.second, local.out);
		}
		ApplyDocument(bind, local.unnamed, local.out);
		local.documents.clear();
		local.unnamed.clear();
	}
	idx_t count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, local.out.size() - local.offset);
	for (idx_t i = 0; i < count; i++) {
		auto &fields = StructValue::GetChildren(local.out[local.offset + i]);
		for (idx_t c = 0; c < fields.size(); c++) {
			output.data[c].SetValue(i, fields[c]);
		}
	}
	output.SetCardinality(count);
	local.offset += count;
	return local.offset < local.out.size() ? OperatorFinalizeResultType::HAVE_MORE_OUTPUT
	                                       : OperatorFinalizeResultType::FINISHED;
}

} // namespace

void RegisterTableApply(ExtensionLoader &loader) {
	FunctionSignature signature;
	signature.AddPositionalOnly("input", LogicalType::TABLE);
	signature.AddParameter(Identifier("sheet"), LogicalType::ANY);
	signature.AddParameter(Identifier("strict"), LogicalType::BOOLEAN, Value::BOOLEAN(false));
	TableFunction fn("dcs_apply", std::move(signature), nullptr, TableApplyBindFn, TableApplyInitGlobal,
	                 TableApplyInitLocal);
	fn.in_out_function = TableApplyIn;
	fn.in_out_function_final = TableApplyFinal;
	loader.RegisterFunction(fn);
}

} // namespace dmb
} // namespace duckdb
