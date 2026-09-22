cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../..")
# Use the package version even in an uncommitted repository or source archive.
file(READ "${CMAKE_CURRENT_LIST_DIR}/../../../library.json" EEPROM24CXX_MANIFEST)
string(REGEX MATCH "\"version\"[ \t\r\n]*:[ \t\r\n]*\"([^\"]+)\"" EEPROM24CXX_VERSION_MATCH "${EEPROM24CXX_MANIFEST}")
set(PROJECT_VER "${CMAKE_MATCH_1}")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(eeprom24cxx_basic_cli)
