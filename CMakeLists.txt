cmake_minimum_required(VERSION 3.1.3)
project(scripter C)

include(CheckFunctionExists)

set(CMAKE_C_STANDARD 11)

if(MINGW)
	add_definitions(-DMINGW_HAS_SECURE_API)
endif()

add_definitions(-Wall -Wsign-compare -Wno-unused-result)

check_function_exists(openpty UTIL_IN_LIBC)
if(NOT UTIL_IN_LIBC)
	find_library(UTIL_LIBRARY util)
endif()

add_executable(scripter scripter.c)
target_link_libraries(scripter ${UTIL_LIBRARY})
