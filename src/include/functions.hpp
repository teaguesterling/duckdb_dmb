#pragma once

#include "duckdb.hpp"

namespace duckdb {
class ExtensionLoader;

namespace dmb {
void RegisterFunctions(ExtensionLoader &loader);
} // namespace dmb
} // namespace duckdb
