cmake_minimum_required(VERSION 3.20)
project(MouseClickStats VERSION 0.3.5 LANGUAGES CXX RC)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT WIN32)
  message(FATAL_ERROR "MouseClickStats requires Windows 10 1809 or later")
endif()
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
if(MSVC)
  add_compile_options(/W4 /utf-8 /permissive- /EHsc)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
  add_link_options(-static -static-libgcc -static-libstdc++)
endif()
add_library(mouse_core STATIC
  src/core/counters.cpp
  src/core/input_merger.cpp
  src/core/dashboard_model.cpp
  src/platform/windows_helpers.cpp
  src/platform/privileges.cpp
  src/platform/startup.cpp
  src/storage/sqlite_store.cpp
  src/collector/persistence.cpp)
target_include_directories(mouse_core PUBLIC src)
target_link_libraries(mouse_core PUBLIC user32 shell32 advapi32 ole32 oleaut32 uuid taskschd)
add_executable(MouseClickStats WIN32 src/collector/main.cpp src/collector/collector.rc)
target_include_directories(MouseClickStats PRIVATE src/collector)
target_link_libraries(MouseClickStats PRIVATE mouse_core)
if(MINGW)
  target_link_options(MouseClickStats PRIVATE -municode)
endif()
enable_testing()
add_executable(mouse_tests tests/core_tests.cpp)
target_link_libraries(mouse_tests PRIVATE mouse_core)
add_test(NAME counters_and_storage COMMAND mouse_tests)
# The optional WinUI 3 dashboard uses MSBuild/C++/WinRT; see scripts/build.ps1.

