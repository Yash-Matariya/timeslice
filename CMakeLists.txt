cmake_minimum_required(VERSION 3.20)
project(timeslice VERSION 0.1.0 LANGUAGES C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options($<$<COMPILE_LANGUAGE:C>:-Wall> $<$<COMPILE_LANGUAGE:C>:-Wextra> $<$<COMPILE_LANGUAGE:C>:-Wshadow>)

# -DTS_SANITIZE=undefined   (UBSan). AddressSanitizer needs fiber annotations
# to follow a stack switch, which this library does not add.
set(TS_SANITIZE "" CACHE STRING "Build with a sanitizer: undefined")
if(TS_SANITIZE STREQUAL "undefined")
  add_compile_options(-fsanitize=undefined -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=undefined)
endif()

add_library(timeslice src/sched.c src/sync.c src/context.c src/context.S src/textbook.c)
target_include_directories(timeslice PUBLIC include PRIVATE src)

add_executable(tsexam tools/tsexam.c)
target_link_libraries(tsexam PRIVATE timeslice)

enable_testing()
add_executable(timeslice_tests
  tests/check.c tests/helpers.c
  tests/test_context.c tests/test_textbook.c tests/test_agree.c
  tests/test_sync.c tests/test_mlfq.c tests/test_real.c)
target_include_directories(timeslice_tests PRIVATE src)
target_link_libraries(timeslice_tests PRIVATE timeslice m)
add_test(NAME timeslice_tests COMMAND timeslice_tests)
