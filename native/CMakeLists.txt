cmake_minimum_required(VERSION 3.22.1)
project(vcam-engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FFMPEG_ROOT ${CMAKE_SOURCE_DIR}/../prebuilt/ffmpeg/${ANDROID_ABI})
set(LOG_ROOT    ${CMAKE_SOURCE_DIR}/../prebuilt/liblog)

# ---------- Motor compartido: lo usan HIDL y AIDL ----------
add_library(vcam_core STATIC
    vcam_config.cpp
    frame_processor.cpp
    video_source.cpp
)
target_include_directories(vcam_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_compile_options(vcam_core PRIVATE -O2 -fno-exceptions)
target_link_libraries(vcam_core
    PUBLIC
    ${FFMPEG_ROOT}/lib/libavformat.so
    ${FFMPEG_ROOT}/lib/libavcodec.so
    ${FFMPEG_ROOT}/lib/libswscale.so
    ${FFMPEG_ROOT}/lib/libavutil.so
    log
)

add_library(gralloc_access STATIC gralloc_access.cpp)
target_include_directories(gralloc_access PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(gralloc_access PUBLIC libnativewindow)

# ---------- Binario provider AIDL (Android 13-16) ----------
file(GLOB_RECURSE AIDL_SOURCES "${CMAKE_SOURCE_DIR}/aidl_gen/src/*.cpp")

add_executable(vcam_provider_aidl
    hal_wrapper_aidl.cpp
    ${AIDL_SOURCES}
)
target_link_libraries(vcam_provider_aidl
    vcam_core
    gralloc_access
    libbinder_ndk
    libnativewindow      # AHardwareBuffer_lock para gralloc
    libcamera_metadata    # NDK camera metadata (fillStaticMetadata)
    log
    c++_static
    c++abi_static
)
target_include_directories(vcam_provider_aidl PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/aidl_gen/include
)
target_compile_options(vcam_provider_aidl PRIVATE -O2 -Wall -Wno-error)