#define DUCKDB_EXTENSION_MAIN

#include "dmb_extension.hpp"
#include "functions.hpp"
#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "tree_sitter/api.h"

extern "C" const TSLanguage *tree_sitter_css(void);

namespace duckdb {

static constexpr const char *DMB_VERSION = "0.1.0";

static void VersionFun(DataChunk &args, ExpressionState &state, Vector &result) {
	result.Reference(Value(DMB_VERSION), count_t(args.size()));
}

static void ParserAbiFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto abi = static_cast<int32_t>(ts_language_abi_version(tree_sitter_css()));
	result.Reference(Value::INTEGER(abi), count_t(args.size()));
}

static void LoadInternal(ExtensionLoader &loader) {
	loader.RegisterFunction(ScalarFunction("dcs_version", {}, LogicalType::VARCHAR, VersionFun));
	loader.RegisterFunction(ScalarFunction("dcs_parser_abi", {}, LogicalType::INTEGER, ParserAbiFun));
	dmb::RegisterFunctions(loader);
}

void DmbExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string DmbExtension::Name() {
	return "dmb";
}

std::string DmbExtension::Version() const {
#ifdef EXT_VERSION_DMB
	return EXT_VERSION_DMB;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(dmb, loader) {
	duckdb::LoadInternal(loader);
}
}
