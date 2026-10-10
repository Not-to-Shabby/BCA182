cmake_minimum_required(VERSION 3.13.1)
include($ENV{ZEPHYR_BASE}/cmake/app/boilerplate.cmake NO_POLICY_SCOPE)
project(Side_Quest_for_LAB_2_Karaoke_Player)

FILE(GLOB app_sources ../src/*.c ../src/*.cpp)
target_sources(app PRIVATE ${app_sources})
target_include_directories(app PRIVATE ../include)

target_compile_options(app PRIVATE -ffast-math -ffp-contract=fast)
