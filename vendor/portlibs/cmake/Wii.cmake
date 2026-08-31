# Local stand-in for devkitPro's wii-cmake package (not installed on this
# machine). ogc-common.cmake already knows NintendoWii; this only names it.
cmake_minimum_required(VERSION 3.13)

if(NOT CMAKE_SYSTEM_NAME)
	set(CMAKE_SYSTEM_NAME NintendoWii)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")
if(NOT DEFINED ENV{DEVKITPRO})
	message(FATAL_ERROR "DEVKITPRO is not set; install devkitPro (see README)")
endif()
include($ENV{DEVKITPRO}/cmake/ogc-common.cmake)
