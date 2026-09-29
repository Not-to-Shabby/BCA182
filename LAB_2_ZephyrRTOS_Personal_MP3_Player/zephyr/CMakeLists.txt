cmake_minimum_required(VERSION 3.13.1)
include($ENV{ZEPHYR_BASE}/cmake/app/boilerplate.cmake NO_POLICY_SCOPE)
project(LAB_2_ZephyrRTOS_Personal_MP3_Player)

FILE(GLOB app_sources ../src/*.c ../src/*.cpp)
target_sources(app PRIVATE ${app_sources})
target_include_directories(app PRIVATE ../include ../reference)
