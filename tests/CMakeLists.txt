find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(ZLIB)

function(bartap_add_test name source)
    add_executable(${name} ${source} canon.c)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(${name} PRIVATE bartap ${ARGN})
    target_compile_options(${name} PRIVATE ${BARTAP_WARNING_FLAGS})
    set_target_properties(${name} PROPERTIES C_EXTENSIONS OFF)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

bartap_add_test(test_foundation unit/test_foundation.c)
bartap_add_test(test_canon unit/test_canon.c)

# Differential tests are evidence the robustness claim rests on (R35, R36), so a missing reference
# implementation is an error rather than a silent skip.
if(NOT ZLIB_FOUND)
    message(FATAL_ERROR "zlib is required for the differential tests")
endif()
bartap_add_test(test_crc_zlib diff/test_crc_zlib.c ZLIB::ZLIB)
bartap_add_test(test_stream unit/test_stream.c)
bartap_add_test(test_directory unit/test_directory.c)
bartap_add_test(test_bytes unit/test_bytes.c)
bartap_add_test(test_inflate_zlib diff/test_inflate_zlib.c ZLIB::ZLIB)

# Real zip files, against Python's zipfile (R36).
add_executable(dump_directory diff/dump_directory.c)
target_include_directories(dump_directory PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(dump_directory PRIVATE bartap)
target_compile_options(dump_directory PRIVATE ${BARTAP_WARNING_FLAGS})
set_target_properties(dump_directory PROPERTIES C_EXTENSIONS OFF)
add_test(NAME test_directory_zipfile
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/diff/check_directory_zipfile.py
            $<TARGET_FILE:dump_directory>)

# The compressed-input size must not change the output (R33). One byte puts a refill at every
# possible point, so these rebuild zip.c with that size instead of using the library's.
function(bartap_add_input1_test name source)
    add_executable(${name} ${source} ${PROJECT_SOURCE_DIR}/src/zip.c)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src)
    target_compile_definitions(${name} PRIVATE BARTAP_INPUT_BUF=1)
    target_link_libraries(${name} PRIVATE ${ARGN})
    target_compile_options(${name} PRIVATE ${BARTAP_WARNING_FLAGS})
    set_target_properties(${name} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

bartap_add_input1_test(test_stream_input1 unit/test_stream.c)
bartap_add_input1_test(test_inflate_zlib_input1 diff/test_inflate_zlib.c ZLIB::ZLIB)

# Sanitizer runtimes add symbols of their own, so the check only means something on a plain build.
if(NOT BARTAP_SANITIZE)
    add_test(NAME symbols
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/check_symbols.py
                ${CMAKE_NM} $<TARGET_FILE:bartap>)

    # The check itself: it has to name a forbidden call in a library that makes one.
    add_library(symcheck_bad STATIC EXCLUDE_FROM_ALL symcheck_bad.c)
    add_test(NAME symbols_build_bad
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target symcheck_bad)
    set_tests_properties(symbols_build_bad PROPERTIES FIXTURES_SETUP symcheck_bad_built)
    add_test(NAME symbols_catches_forbidden
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/check_symbols.py
                ${CMAKE_NM} $<TARGET_FILE:symcheck_bad>)
    set_tests_properties(symbols_catches_forbidden PROPERTIES
        FIXTURES_REQUIRED symcheck_bad_built
        PASS_REGULAR_EXPRESSION "forbidden symbol: strlen")
endif()
