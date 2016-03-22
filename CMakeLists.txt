project( eso-unpack C CXX )

cmake_minimum_required( VERSION 2.8 )

include_directories( ${PROJECT_SOURCE_DIR} )
include_directories( ${PROJECT_SOURCE_DIR}/src )

list( APPEND CMAKE_CXX_FLAGS "-std=c++11" )

add_executable( ${PROJECT_NAME}
	src/lookup2.c
	src/esounpack.cpp
	)

target_link_libraries( ${PROJECT_NAME}
	z
	)
