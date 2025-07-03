cmake_minimum_required(VERSION 2.8) 
project(openfec C)

enable_testing()

if(PROFILING STREQUAL "ON")
    message(STATUS "Profiling enabled")
endif()

if(DEBUG STREQUAL "ON")
    add_definitions(-DOF_DEBUG)
    set(CMAKE_BUILD_TYPE Debug)
    message(STATUS "Debug mode ON")
else()
    set(CMAKE_BUILD_TYPE Release)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2")
    message(STATUS "Debug mode OFF")
endif()

if(LIBRARY_OUTPUT_PATH)
    message(STATUS "Using custom LIBRARY_OUTPUT_PATH: ${LIBRARY_OUTPUT_PATH}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${LIBRARY_OUTPUT_PATH})
endif()

mark_as_advanced(
    LIBRARY_OUTPUT_PATH
    EXECUTABLE_OUTPUT_PATH
)

message(STATUS "Install include dir: ${CMAKE_INSTALL_LIBDIR}")
link_directories(${CMAKE_ARCHIVE_OUTPUT_DIRECTORY})

add_subdirectory(src)

install(TARGETS openfec
        ARCHIVE DESTINATION lib)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/src/
        DESTINATION include
        FILES_MATCHING PATTERN "*.h")
