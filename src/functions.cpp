#include "functions.hpp"
#include "sheet.hpp"
#include "block_tree.hpp"
#include "matcher.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
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

// ---------------------------------------------------------------------------
// dcs_select(blocks, selector): the element_orders of the blocks the selector matches
// ---------------------------------------------------------------------------

struct SelectBindData : public FunctionData {
	BlockFields fields;
	explicit SelectBindData(BlockFields f) : fields(f) {
	}
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<SelectBindData>(fields);
	}
	bool Equals(const FunctionData &other) const override {
		return true;
	}
};

void SelectResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &type = input.GetArgumentType(0);
	if (type.id() == LogicalTypeId::SQLNULL || type.id() == LogicalTypeId::UNKNOWN) {
		return;
	}
	ResolveBlockFields(type, "dcs_select");
	input.GetBoundFunction().GetArguments()[0] = type;
}

unique_ptr<FunctionData> SelectBind(BindScalarFunctionInput &input) {
	auto &type = input.GetBoundFunction().GetArguments()[0];
	if (type.id() != LogicalTypeId::LIST) {
		return make_uniq<SelectBindData>(BlockFields {});
	}
	return make_uniq<SelectBindData>(ResolveBlockFields(type, "dcs_select"));
}

//! Compile a selector for a SQL caller: refuse it by name rather than match nothing.
ComplexSelector CompileOrThrow(const string &text, const string &function_name) {
	ComplexSelector sel;
	string pe;
	vector<Diagnostic> diags;
	if (!CompileSelectorText(text, sel, pe, diags, SourcePos {1, 1}) || !pe.empty()) {
		string message = diags.empty() ? "a pseudo-element can't be selected" : diags[0].message;
		throw InvalidInputException("%s: %s", function_name, message);
	}
	return sel;
}

void SelectFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func = state.expr.Cast<BoundFunctionExpression>();
	auto &bind = func.BindInfo()->Cast<SelectBindData>();
	for (idx_t row = 0; row < args.size(); row++) {
		auto blocks = args.data[0].GetValue(row);
		auto selector = args.data[1].GetValue(row);
		if (blocks.IsNull() || selector.IsNull()) {
			result.SetValue(row, Value(result.GetType()));
			continue;
		}
		auto sel = CompileOrThrow(selector.ToString(), "dcs_select");
		auto tree = BlockTree::FromList(blocks, bind.fields);
		Matcher matcher(tree);
		vector<Value> hits;
		for (idx_t i = 0; i < tree.nodes.size(); i++) {
			if (matcher.Matches(sel, i)) {
				hits.push_back(Value::INTEGER(tree.nodes[i].element_order));
			}
		}
		result.SetValue(row, Value::LIST(LogicalType::INTEGER, std::move(hits)));
	}
}

} // namespace

void RegisterFunctions(ExtensionLoader &loader) {
	TableFunction rules("dcs_rules", {LogicalType::VARCHAR}, RulesScan, RulesBind, RulesInit);
	loader.RegisterFunction(rules);

	ScalarFunction select("dcs_select", {LogicalType::ANY, LogicalType::VARCHAR},
	                      LogicalType::LIST(LogicalType::INTEGER), SelectFun);
	select.SetResolveTypesCallback(SelectResolveTypes);
	select.SetBindCallback(SelectBind);
	select.SetFallible();
	loader.RegisterFunction(select);
}

} // namespace dmb
} // namespace duckdb
