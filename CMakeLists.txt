cmake_minimum_required(VERSION 3.10)
project(ttygg C CXX)

set(SOURCES main.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Android")
  list(APPEND SOURCES linux_nonstd_baud.cpp)
endif()
add_executable(ttygg ${SOURCES})
target_compile_features(ttygg PRIVATE cxx_std_11)
if(APPLE)
  find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
  target_link_libraries(ttygg PRIVATE ${IOKIT_FRAMEWORK})
endif()

# Release: strip symbol table from the installed binary (smaller; "file" shows stripped).
# Single-config generators only (Ninja/Make + -DCMAKE_BUILD_TYPE=Release). Xcode multi-config:
#   cmake --build build --config Release && strip build/Release/ttygg
if(UNIX)
  if(NOT CMAKE_STRIP)
    find_program(CMAKE_STRIP NAMES strip llvm-strip)
  endif()
  string(TOUPPER "${CMAKE_BUILD_TYPE}" _ttygg_cfg)
  if(CMAKE_STRIP AND _ttygg_cfg STREQUAL "RELEASE")
    add_custom_command(
      TARGET ttygg
      POST_BUILD
      COMMAND "${CMAKE_STRIP}" "$<TARGET_FILE:ttygg>"
      COMMENT "Stripping ttygg")
  endif()
endif()
