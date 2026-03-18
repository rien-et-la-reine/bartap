add_library( bartap
    bartap.c
    )

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2 CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(bartap C CXX ASM)

pico_sdk_init()

# Add FatFs
add_subdirectory(ff16/source)

target_link_libraries(bartap
    pico_stdlib
    hardware_spi
    hardware_pio
    hardware_dma
    ff16
)

target_include_directories(bartap PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")