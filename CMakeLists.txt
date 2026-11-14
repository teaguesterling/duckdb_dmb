cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME dmb)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include)

set(EXTENSION_SOURCES
    src/dmb_extension.cpp)

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
