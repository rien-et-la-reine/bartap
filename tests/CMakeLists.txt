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
bartap_add_test(test_xml unit/test_xml.c)
bartap_add_test(test_package unit/test_package.c)
bartap_add_test(test_text unit/test_text.c)
bartap_add_test(test_seek unit/test_seek.c)
bartap_add_test(test_toc unit/test_toc.c)
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

# Builds sources straight from src/ with limits other than the library's. defines is a list such as
# "BARTAP_XML_READAHEAD=1"; the remaining arguments are source files.
function(bartap_add_sized_executable name defines)
    add_executable(${name} ${ARGN} ${PROJECT_SOURCE_DIR}/src/zip.c ${PROJECT_SOURCE_DIR}/src/xml.c
        ${PROJECT_SOURCE_DIR}/src/package.c ${PROJECT_SOURCE_DIR}/src/text.c)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src)
    target_compile_definitions(${name} PRIVATE ${defines})
    target_compile_options(${name} PRIVATE ${BARTAP_WARNING_FLAGS})
    set_target_properties(${name} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
endfunction()

# The tokenizer's read-ahead must not change its tokens (R33). One byte makes every byte a refill.
bartap_add_sized_executable(test_xml_readahead1 "BARTAP_XML_READAHEAD=1" unit/test_xml.c)
add_test(NAME test_xml_readahead1 COMMAND test_xml_readahead1)

# Opening a book through 1-byte buffers everywhere: a refill at every byte, same book (R33).
bartap_add_sized_executable(test_package_buffers1 "BARTAP_XML_READAHEAD=1;BARTAP_INPUT_BUF=1"
    unit/test_package.c)
add_test(NAME test_package_buffers1 COMMAND test_package_buffers1)

# The text layer, TOC and seeks through the same 1-byte buffers: records must not change (R33). The
# seek properties sample every fifth landing point here, the full run is test_seek's.
bartap_add_sized_executable(test_text_buffers1 "BARTAP_XML_READAHEAD=1;BARTAP_INPUT_BUF=1" unit/test_text.c)
add_test(NAME test_text_buffers1 COMMAND test_text_buffers1)
bartap_add_sized_executable(test_toc_buffers1 "BARTAP_XML_READAHEAD=1;BARTAP_INPUT_BUF=1" unit/test_toc.c)
add_test(NAME test_toc_buffers1 COMMAND test_toc_buffers1)
bartap_add_sized_executable(test_seek_buffers1 "BARTAP_XML_READAHEAD=1;BARTAP_INPUT_BUF=1" unit/test_seek.c)
add_test(NAME test_seek_buffers1 COMMAND test_seek_buffers1 5)

# The same, across builds: every build hashes the same documents and all must agree.
add_executable(xml_hash prop/xml_hash.c)
target_include_directories(xml_hash PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(xml_hash PRIVATE bartap)
target_compile_options(xml_hash PRIVATE ${BARTAP_WARNING_FLAGS})
set_target_properties(xml_hash PROPERTIES C_EXTENSIONS OFF)
bartap_add_sized_executable(xml_hash_readahead1 "BARTAP_XML_READAHEAD=1" prop/xml_hash.c)
bartap_add_sized_executable(xml_hash_readahead7 "BARTAP_XML_READAHEAD=7" prop/xml_hash.c)
add_test(NAME xml_readahead_agree
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/prop/compare_outputs.py
            $<TARGET_FILE:xml_hash> $<TARGET_FILE:xml_hash_readahead1>
            $<TARGET_FILE:xml_hash_readahead7>)

# Generated EPUB fixtures (phase 10). gen.py is the source; the books are built into the build tree,
# and configuring again picks up a new fixture's name.
set(BARTAP_FIXTURE_GEN ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/gen.py)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${BARTAP_FIXTURE_GEN})
execute_process(COMMAND ${Python3_EXECUTABLE} ${BARTAP_FIXTURE_GEN} --list
    OUTPUT_VARIABLE fixture_names OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
string(REPLACE "\n" ";" fixture_names "${fixture_names}")
set(BARTAP_FIXTURE_DIR ${CMAKE_CURRENT_BINARY_DIR}/fixtures)
set(fixture_files "")
foreach(fixture ${fixture_names})
    list(APPEND fixture_files ${BARTAP_FIXTURE_DIR}/${fixture}.epub)
endforeach()
add_custom_command(OUTPUT ${fixture_files}
    COMMAND Python3::Interpreter ${BARTAP_FIXTURE_GEN} ${BARTAP_FIXTURE_DIR}
    DEPENDS ${BARTAP_FIXTURE_GEN}
    COMMENT "Generating EPUB fixtures")
add_custom_target(bartap_fixtures ALL DEPENDS ${fixture_files})

# The dump of every fixture against the expected lines, and the pairs that must agree (R26, R51). The
# dump checks its own agreements too: directory modes, stream and memory, text buffer sizes. After an
# intended change in output, refresh the file with check.py's --update, then bump the bookmark
# version if text or block boundaries changed (R12).
add_executable(fixture_dump fixtures/dump.c canon.c)
target_include_directories(fixture_dump PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fixture_dump PRIVATE bartap)
target_compile_options(fixture_dump PRIVATE ${BARTAP_WARNING_FLAGS})
set_target_properties(fixture_dump PROPERTIES C_EXTENSIONS OFF)
add_dependencies(fixture_dump bartap_fixtures)
add_test(NAME fixtures_expected
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/check.py
            $<TARGET_FILE:fixture_dump> ${BARTAP_FIXTURE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/expected.txt)

# The same dump through 1-byte buffers and an odd read-ahead: every line must match (R33).
bartap_add_sized_executable(fixture_dump_buffers1 "BARTAP_XML_READAHEAD=1;BARTAP_INPUT_BUF=1"
    fixtures/dump.c canon.c)
bartap_add_sized_executable(fixture_dump_readahead7 "BARTAP_XML_READAHEAD=7;BARTAP_INPUT_BUF=7"
    fixtures/dump.c canon.c)
add_test(NAME fixtures_buffers_agree
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/prop/compare_outputs.py
            $<TARGET_FILE:fixture_dump> $<TARGET_FILE:fixture_dump_buffers1>
            $<TARGET_FILE:fixture_dump_readahead7> -- ${fixture_files})

# P1 and P2 at landing points through every fixture, and every TOC entry followed (R13, R48).
add_executable(test_fixture_seek fixtures/seek.c canon.c)
target_include_directories(test_fixture_seek PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_fixture_seek PRIVATE bartap)
target_compile_options(test_fixture_seek PRIVATE ${BARTAP_WARNING_FLAGS})
set_target_properties(test_fixture_seek PROPERTIES C_EXTENSIONS OFF)
add_dependencies(test_fixture_seek bartap_fixtures)
add_test(NAME test_fixture_seek COMMAND test_fixture_seek ${fixture_files})

# Fuzz targets, replayed over generated documents in every run, at two read-aheads. Pass files to
# these executables to replay a corpus or a crash.
bartap_add_sized_executable(fuzz_xml_replay "BARTAP_XML_READAHEAD=256" fuzz/fuzz_xml.c fuzz/fuzz_main.c)
bartap_add_sized_executable(fuzz_xml_replay_readahead1 "BARTAP_XML_READAHEAD=1"
    fuzz/fuzz_xml.c fuzz/fuzz_main.c)
add_test(NAME fuzz_xml_replay COMMAND fuzz_xml_replay)
add_test(NAME fuzz_xml_replay_readahead1 COMMAND fuzz_xml_replay_readahead1)
bartap_add_sized_executable(fuzz_open_replay "BARTAP_XML_READAHEAD=256" fuzz/fuzz_open.c fuzz/fuzz_main.c)
bartap_add_sized_executable(fuzz_open_replay_buffers1 "BARTAP_XML_READAHEAD=1;BARTAP_INPUT_BUF=1"
    fuzz/fuzz_open.c fuzz/fuzz_main.c)
add_test(NAME fuzz_open_replay COMMAND fuzz_open_replay)
add_test(NAME fuzz_open_replay_buffers1 COMMAND fuzz_open_replay_buffers1)
bartap_add_sized_executable(fuzz_text_replay "BARTAP_XML_READAHEAD=256" fuzz/fuzz_text.c fuzz/fuzz_main.c)
bartap_add_sized_executable(fuzz_text_replay_buffers1 "BARTAP_XML_READAHEAD=1;BARTAP_INPUT_BUF=1"
    fuzz/fuzz_text.c fuzz/fuzz_main.c)
add_test(NAME fuzz_text_replay COMMAND fuzz_text_replay)
add_test(NAME fuzz_text_replay_buffers1 COMMAND fuzz_text_replay_buffers1)

# libFuzzer builds, with clang: cmake -DBARTAP_FUZZ=ON -DCMAKE_C_COMPILER=clang, then run
# build/tests/fuzz_xml corpus/ (and fuzz_xml_readahead1, fuzz_open, fuzz_text). Then replay the tokenizer's
# corpus through xml_hash and xml_hash_readahead1 with compare_outputs.py to check R33 on what the
# fuzzer found.
if(BARTAP_FUZZ)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "BARTAP_FUZZ needs clang for libFuzzer")
    endif()
    foreach(size 256 1)
        set(name fuzz_xml)
        if(NOT size EQUAL 256)
            set(name fuzz_xml_readahead${size})
        endif()
        bartap_add_sized_executable(${name} "BARTAP_XML_READAHEAD=${size}" fuzz/fuzz_xml.c)
        target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined -g)
        target_link_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
    endforeach()
    foreach(target fuzz_open fuzz_text)
        bartap_add_sized_executable(${target} "BARTAP_XML_READAHEAD=256" fuzz/${target}.c)
        target_compile_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined -g)
        target_link_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
    endforeach()
endif()

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
