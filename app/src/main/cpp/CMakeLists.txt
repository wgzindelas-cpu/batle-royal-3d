cmake_minimum_required(VERSION 3.22.1)
project(native-game LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NATIVE_APP_GLUE_DIR "${CMAKE_ANDROID_NDK}/sources/android/native_app_glue")
add_library(native-game SHARED main.cpp "${NATIVE_APP_GLUE_DIR}/android_native_app_glue.c")
target_include_directories(native-game PRIVATE "${NATIVE_APP_GLUE_DIR}")
target_link_libraries(native-game android log EGL GLESv2 OpenSLES)