cmake_minimum_required(VERSION 3.12)

project(coreaudio-encoder-linux VERSION 0.1.0)

# Replace `Your Name Here` with the name (yours or your organization's) you want
# to see as the author of the plugin (in the plugin's metadata itself and in the installers)
set(PLUGIN_AUTHOR "Norihiro Kamae")

# Replace `com.example.obs-plugin-template` with a unique Bundle ID for macOS releases
# (used both in the installer and when submitting the installer for notarization)
set(PLUGIN_URL "TBD")
set(ID_PREFIX "")

# Replace `me@contoso.com` with the maintainer email address you want to put in Linux packages
set(LINUX_MAINTAINER_EMAIL "norihiro@nagater.net")

option(ENV_WINEPATH "Set environment variable 'WINEPATH'" "")
option(WINE_EXE_PATH "Absolute path to 'wine'" "")

# TAKE NOTE: No need to edit things past this point

if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_CURRENT_SOURCE_DIR})
	find_package(libobs REQUIRED)
	include(cmake/ObsPluginHelpers.cmake)
endif()

configure_file(
	src/plugin-macros.h.in
	plugin-macros.generated.h
)

option(ENCODER_PROC_HEADER_ONLY "" ON)
set(ENCODER_PROC_HEADER_ONLY ON)
add_subdirectory(encoder-proc)

set(PLUGIN_SOURCES
	src/plugin-main.c
	src/aac-encoder.cc
	src/run-proc.c
)

add_library(${PROJECT_NAME} MODULE ${PLUGIN_SOURCES})

target_link_libraries(${PROJECT_NAME}
	OBS::libobs
)

target_include_directories(${PROJECT_NAME} PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_BINARY_DIR}
)

if(OS_LINUX)
	target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
endif()

setup_plugin_target(${PROJECT_NAME})

if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_CURRENT_SOURCE_DIR})
	configure_file(
		ci/ci_includes.sh.in
		ci/ci_includes.generated.sh
	)
endif()
