cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME dmb)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME} C CXX)
include_directories(src/include)

# Vendored tree-sitter runtime (submodule, sitting_duck's pin) and the generated
# tree-sitter-css parser. See third_party/tree-sitter-css/PROVENANCE.md.
include_directories(third_party/tree-sitter/lib/include)
set(TREE_SITTER_SOURCES
    third_party/tree-sitter/lib/src/lib.c
    third_party/tree-sitter-css/src/parser.c
    third_party/tree-sitter-css/src/scanner.c)
set_source_files_properties(${TREE_SITTER_SOURCES} PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter/lib/src;${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter-css/src")

set(EXTENSION_SOURCES
    src/dmb_extension.cpp
    src/functions.cpp
    src/sheet_parser.cpp
    src/selector.cpp
    src/block_tree.cpp
    src/matcher.cpp
    src/values.cpp
    src/apply.cpp
    src/dcs_value.cpp
    src/readers.cpp
    src/apply_table.cpp
    ${TREE_SITTER_SOURCES})

# WASM: the loadable module is linked by a separate emcc -sSIDE_MODULE=2 step
# (duckdb/extension/extension_build_tools.cmake) that links only this
# variable's libraries, and reads it in build_loadable_extension, so it is set
# first. duckdb-wasm exports DuckDB's Regex wrapper but not the duckdb_re2::RE2
# class that dmb calls (-dcs-replace, :matches), so dmb links its own copy.
# A literal path: a $<TARGET_FILE:...> genexpr resolves empty in that step
# (duckdb_webbed found the same). test/wasm/check_wasm_imports.mjs checks it.
if(EMSCRIPTEN)
  set(DUCKDB_EXTENSION_DMB_LINKED_LIBS
      "${CMAKE_BINARY_DIR}/third_party/re2/libduckdb_re2.a"
      CACHE STRING "" FORCE)
endif()

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# MSVC needs /std:c++17 for duckdb's vendored fmt (same reason as duck_block_utils).
if(MSVC)
  target_compile_options(${EXTENSION_NAME}
                         PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/std:c++17>")
  target_compile_options(${LOADABLE_EXTENSION_NAME}
                         PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/std:c++17>")
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
