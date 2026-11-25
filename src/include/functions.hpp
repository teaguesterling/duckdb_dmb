#pragma once

#include "duckdb.hpp"

namespace duckdb {
class ExtensionLoader;

namespace dmb {
void RegisterFunctions(ExtensionLoader &loader);
//! FROM dcs_apply((subquery), sheet [, strict]) (apply_table.cpp).
void RegisterTableApply(ExtensionLoader &loader);
} // namespace dmb
} // namespace duckdb
