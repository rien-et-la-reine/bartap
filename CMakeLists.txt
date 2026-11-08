cmake_minimum_required(VERSION 3.21)
project(bartap LANGUAGES C)

# For clangd, which finds build/compile_commands.json by itself.
if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

option(BARTAP_BUILD_TESTS "Build Bartap's host tests" ${PROJECT_IS_TOP_LEVEL})
option(BARTAP_SANITIZE "Build with ASan and UBSan" OFF)
option(BARTAP_FUZZ "Build libFuzzer targets (clang)" OFF)

# Output-neutral limits (R33). PUBLIC below, so the library and its callers always agree.
set(BARTAP_INPUT_BUF 512 CACHE STRING "Compressed bytes read per inflate refill")
set(BARTAP_XML_READAHEAD 256 CACHE STRING "XML tokenizer lookahead")

if(BARTAP_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

set(BARTAP_WARNING_FLAGS
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wconversion;-pedantic>
    $<$<AND:$<BOOL:${PROJECT_IS_TOP_LEVEL}>,$<C_COMPILER_ID:GNU,Clang>>:-Werror>)

add_library(bartap STATIC src/zip.c src/xml.c src/package.c src/text.c)
target_include_directories(bartap PUBLIC include PRIVATE src)
target_compile_definitions(bartap PUBLIC
    BARTAP_INPUT_BUF=${BARTAP_INPUT_BUF}
    BARTAP_XML_READAHEAD=${BARTAP_XML_READAHEAD})
target_compile_features(bartap PUBLIC c_std_11)
target_compile_options(bartap PRIVATE ${BARTAP_WARNING_FLAGS})
# Some toolchains enable this by default, and it needs __stack_chk_fail, which R34 and R35 rule out.
target_compile_options(bartap PRIVATE $<$<C_COMPILER_ID:GNU,Clang>:-fno-stack-protector>)
set_target_properties(bartap PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

if(BARTAP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
