cmake_minimum_required(VERSION 3.20)
project(uaw_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wall> $<$<COMPILE_LANGUAGE:CXX>:-Wextra> $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>)
elseif(MSVC)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/W3> $<$<COMPILE_LANGUAGE:CXX>:/utf-8> $<$<COMPILE_LANGUAGE:CXX>:/permissive-> $<$<COMPILE_LANGUAGE:CXX>:/Zc:preprocessor>)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
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
  engine/src/assets.cpp
  engine/src/nav.cpp
  engine/src/sim.cpp
    engine/src/economy.cpp
    engine/src/effects.cpp
    engine/src/abilities.cpp
    engine/src/superweapons.cpp
    engine/src/particles.cpp
    engine/src/sfx.cpp
    engine/src/garrison.cpp
    engine/src/flow.cpp
    engine/src/luavm.cpp
    engine/src/luacompile.cpp
    engine/src/lualib.cpp
    engine/src/scripts.cpp
    engine/src/research.cpp
    engine/src/ai.cpp
  engine/src/combat.cpp
  engine/src/techtree.cpp
  engine/src/strings.cpp
)
target_include_directories(alamo PUBLIC engine/include)
find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
  # Windows and other systems without a system zlib: build it from source.
  include(FetchContent)
  set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG v1.3.1
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(zlib)
  target_include_directories(zlibstatic INTERFACE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
  add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()
find_package(Threads REQUIRED)
target_link_libraries(alamo PUBLIC ZLIB::ZLIB Threads::Threads)

add_executable(simtool tools/simtool.cpp)
target_link_libraries(simtool PRIVATE alamo)

add_executable(maptool tools/maptool.cpp)
target_link_libraries(maptool PRIVATE alamo)

add_executable(alotool tools/alotool.cpp)
target_link_libraries(alotool PRIVATE alamo)

add_executable(test_sim tests/test_sim.cpp)
target_link_libraries(test_sim PRIVATE alamo)

add_executable(test_formats tests/test_formats.cpp)
target_link_libraries(test_formats PRIVATE alamo)

add_executable(megtool tools/megtool.cpp)
target_link_libraries(megtool PRIVATE alamo)

add_executable(objdump tools/objdump.cpp)
target_link_libraries(objdump PRIVATE alamo)

add_executable(test_abilities tests/test_abilities.cpp)
target_link_libraries(test_abilities PRIVATE alamo)

add_executable(test_xml tests/test_xml.cpp)
target_link_libraries(test_xml PRIVATE alamo)

enable_testing()
add_test(NAME xml_and_gamedata COMMAND test_xml)
add_test(NAME alo_dds_gltf COMMAND test_formats)
add_test(NAME nav_sim COMMAND test_sim)
add_test(NAME effects_abilities COMMAND test_abilities)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME meg_roundtrip
    COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tests/test_meg.py $<TARGET_FILE:megtool>)
endif()

# ---------------------------------------------------------------- client (viewer)
option(UAW_BUILD_CLIENT "Build the SDL3/OpenGL viewer" ON)
if(UAW_BUILD_CLIENT)
  find_package(SDL3 CONFIG QUIET)
  if(NOT SDL3_FOUND)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    # Point FETCHCONTENT_SOURCE_DIR_SDL3 at an existing checkout to skip the download.
    FetchContent_Declare(SDL3
      GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
      GIT_TAG release-3.2.24
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL3)
  endif()

  add_executable(uaw-viewer WIN32
    client/viewer.cpp
    client/renderer.cpp
    client/scene.cpp
    client/units.cpp
    client/hud.cpp
    client/menu.cpp
    client/particles.cpp
    client/audio.cpp
    client/savegame.cpp
    client/net.cpp
    client/lockstep.cpp
    client/font.cpp
    client/gl.cpp)
  target_link_libraries(uaw-viewer PRIVATE alamo SDL3::SDL3)
  if(WIN32)
    target_link_libraries(uaw-viewer PRIVATE ws2_32)
  endif()
  target_include_directories(uaw-viewer PRIVATE third_party)
endif()
