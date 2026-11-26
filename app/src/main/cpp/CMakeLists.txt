cmake_minimum_required(VERSION 3.22.1)

project(67xFramesNative LANGUAGES CXX)

add_library(native-lib SHARED native-lib.cpp)

find_library(ANDROID_LIB android)
find_library(LOG_LIB log)
find_library(VULKAN_LIB vulkan)

target_link_libraries(
    native-lib
    PRIVATE
    ${ANDROID_LIB}
    ${LOG_LIB}
    ${VULKAN_LIB}
)
