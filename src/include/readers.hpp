#pragma once

#include "duckdb.hpp"

namespace duckdb {
class ExtensionLoader;

namespace dmb {
//! read_dcs_objects(globs) and read_dcs(globs).
void RegisterReaders(ExtensionLoader &loader);
} // namespace dmb
} // namespace duckdb
