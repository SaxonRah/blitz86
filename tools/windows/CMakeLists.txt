cmake_minimum_required(VERSION 3.20)
project(blitz86_windows_tests LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_executable(sst_interp
  "${CMAKE_CURRENT_LIST_DIR}/../../src/interp.c"
  "${CMAKE_CURRENT_LIST_DIR}/../../tests/sst_interp.c"
)
target_include_directories(sst_interp PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../include")
if(MSVC)
  target_compile_options(sst_interp PRIVATE /W3 /O2)
endif()
enable_testing()
add_test(NAME silicon_quick COMMAND sst_interp "${CMAKE_CURRENT_LIST_DIR}/../../sst/quick.bin")
add_test(NAME silicon_full COMMAND sst_interp "${CMAKE_CURRENT_LIST_DIR}/../../sst/all.bin")
