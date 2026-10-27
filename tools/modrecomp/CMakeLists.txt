cmake_minimum_required(VERSION 3.20)
project(modrecomp CXX C)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# N64Recomp's libraries, from the N64Recomp-DC submodule.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../N64Recomp ${CMAKE_CURRENT_BINARY_DIR}/N64Recomp EXCLUDE_FROM_ALL)

# config.cpp holds Context::from_symbol_file and import_reference_context, as for OfflineModRecomp.
add_executable(modrecomp main.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../N64Recomp/src/config.cpp)
target_link_libraries(modrecomp N64Recomp fmt rabbitizer tomlplusplus::tomlplusplus)
