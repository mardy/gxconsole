cmake_minimum_required(VERSION 3.13)

project(gxconsole LANGUAGES C VERSION 1.0.0)

option(BUILD_EXAMPLE "Build example program" OFF)

include(GNUInstallDirs)

add_library(${PROJECT_NAME} STATIC)

target_sources(${PROJECT_NAME} PRIVATE
	source/fonts/cozette_6x13.c
	source/fonts/tamzen_8x15.c
	source/gxconsole.c
)

target_compile_options(${PROJECT_NAME} PRIVATE -Wall)

target_include_directories(${PROJECT_NAME} PUBLIC
    source
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    PUBLIC_HEADER "source/gxconsole.h"
)

install(TARGETS ${PROJECT_NAME}
    LIBRARY
    ARCHIVE
    RUNTIME
)

if(BUILD_EXAMPLE)
    add_subdirectory(example)
endif()
