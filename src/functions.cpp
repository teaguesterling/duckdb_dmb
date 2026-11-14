#include "functions.hpp"
#include "sheet.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace dmb {

namespace {

// ---------------------------------------------------------------------------
// dcs_rules(sheet): one row per (selector after list-splitting, declaration)
// ---------------------------------------------------------------------------

struct RulesBindData : public TableFunctionData {
	vector<vector<Value>> rows;
};

struct RulesState : public GlobalTableFunctionState {
	idx_t offset = 0;
};

Value OrNull(const string &s) {
	return s.empty() ? Value(LogicalType::VARCHAR) : Value(s);
}

unique_ptr<FunctionData> RulesBind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &types,
                                   vector<Identifier> &names) {
	names = {"rule", "selector", "pseudo_element", "property", "value", "important",
	         "specificity", "order", "media", "line", "column"};
	types = {LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	         LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::LIST(LogicalType::INTEGER),
	         LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::INTEGER};
	auto result = make_uniq<RulesBindData>();
	auto &arg = input.inputs[0];
	if (arg.IsNull()) {
		return std::move(result);
	}
	auto sheet = ParseSheet(arg.GetValue<string>());
	for (auto &rule : sheet.rules) {
		auto &sp = rule.selector.specificity;
		int32_t c = sp.c + (rule.pseudo_element.empty() ? 0 : 1);
		auto spec = Value::LIST(LogicalType::INTEGER, {Value::INTEGER(sp.a), Value::INTEGER(sp.b), Value::INTEGER(c)});
		for (auto &d : rule.declarations) {
			result->rows.push_back({Value::INTEGER(rule.index), Value(rule.selector_text), OrNull(rule.pseudo_element),
			                        Value(d.property), Value(d.value_text), Value::BOOLEAN(d.important), spec,
			                        Value::INTEGER(d.order), OrNull(rule.media_format), Value::INTEGER(d.pos.line),
			                        Value::INTEGER(d.pos.column)});
		}
	}
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> RulesInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<RulesState>();
}

void RulesScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &bind = data.bind_data->Cast<RulesBindData>();
	auto &state = data.global_state->Cast<RulesState>();
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

void RegisterFunctions(ExtensionLoader &loader) {
	TableFunction rules("dcs_rules", {LogicalType::VARCHAR}, RulesScan, RulesBind, RulesInit);
	loader.RegisterFunction(rules);
}

} // namespace dmb
} // namespace duckdb
