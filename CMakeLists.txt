cmake_minimum_required(VERSION 3.20)
project(uaw_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Core engine library: file formats first, simulation and rendering later.
add_library(alamo
  engine/src/meg.cpp
  engine/src/xml.cpp
  engine/src/gamedata.cpp
  engine/src/alo.cpp
  engine/src/dds.cpp
  engine/src/gltf.cpp
  engine/src/ala.cpp
  engine/src/ted.cpp
)
target_include_directories(alamo PUBLIC engine/include)
find_package(ZLIB REQUIRED)
target_link_libraries(alamo PUBLIC ZLIB::ZLIB)

add_executable(maptool tools/maptool.cpp)
target_link_libraries(maptool PRIVATE alamo)

add_executable(alotool tools/alotool.cpp)
target_link_libraries(alotool PRIVATE alamo)

add_executable(test_formats tests/test_formats.cpp)
target_link_libraries(test_formats PRIVATE alamo)

add_executable(megtool tools/megtool.cpp)
target_link_libraries(megtool PRIVATE alamo)

add_executable(objdump tools/objdump.cpp)
target_link_libraries(objdump PRIVATE alamo)

add_executable(test_xml tests/test_xml.cpp)
target_link_libraries(test_xml PRIVATE alamo)

enable_testing()
add_test(NAME xml_and_gamedata COMMAND test_xml)
add_test(NAME alo_dds_gltf COMMAND test_formats)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME meg_roundtrip
    COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tests/test_meg.py $<TARGET_FILE:megtool>)
endif()
