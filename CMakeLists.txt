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
    ${TREE_SITTER_SOURCES})

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
