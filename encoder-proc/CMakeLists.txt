cmake_minimum_required(VERSION 3.12)

project(obs-coreaudio-encoder-proc VERSION 0.1.0)

option(LIBOBS_INC_DIRS "Path to libobs header files for inline functions" "")

configure_file(
	encoder-proc-version.h.in
	encoder-proc-version.h
)

if (ENCODER_PROC_HEADER_ONLY)
	return()
endif()

set(PROC_SOURCES
	encoder-proc.cc
	util.c
)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	add_compile_options(-Wno-multichar)
endif()

add_executable(${PROJECT_NAME}
	${PROC_SOURCES}
)
target_include_directories(${PROJECT_NAME} PRIVATE
	${CMAKE_CURRENT_BINARY_DIR}
	${LIBOBS_INC_DIRS}
)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 17)

if (MSVC)
	add_definitions(/MP /d2FH4-)
endif()
