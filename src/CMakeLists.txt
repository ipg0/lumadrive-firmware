# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)
set(generated_sources ${CMAKE_SOURCE_DIR}/generated/mesh.pb.c)

idf_component_register(
	SRCS ${app_sources} ${generated_sources}
	INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}/generated")
