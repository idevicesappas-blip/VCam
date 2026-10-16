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
target_compile_options(vcam_core PRIVATE -O2 -fno-exceptions-safe)
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
target_link_libraries(gralloc_access PUBLIC libnativewindow libui libgralloctypes libhidlbase)

# ---------- Motor con malloc hooks HIDL (Android 10-12) ----------
if (${ANDROID_SDK} LESS 33)
    # Enlaza los bindings HIDL (los .so existen en /vendor/lib/ del sistema
    # o se precompilan con out/soong/host/linux-x86/bin/hidl-gen)
    add_library(engine_hidl SHARED
        hal_wrapper_hidl.cpp
    )
    target_link_libraries(engine_hidl vcam_core hidlbase log c++_static
        android.hardware.camera.provider@2.4
        android.hardware.camera.provider@2.5
        android.hardware.camera.provider@2.6
        android.hardware.camera.provider@2.7
        android.hardware.camera.device@3.2
        android.hardware.camera.device@3.3
        android.hardware.camera.device@3.4
        android.hardware.camera.device@3.5
        android.hardware.camera.device@3.6
    )
    target_compile_options(engine_hidl PRIVATE -O2)
endif()

# ---------- Binario provider AIDL (Android 13-16) ----------
add_executable(vcam_provider_aidl
    hal_wrapper_aidl.cpp
)
target_link_libraries(vcam_provider_aidl
    vcam_core
    gralloc_access
    android.hardware.camera.provider-V1-ndk
    android.hardware.camera.device-V1-ndk
    android.hardware.common-V2-ndk
    android.hardware.graphics.common-V3-ndk
    libbinder_ndk
    libnativewindow      # AHardwareBuffer_lock para gralloc
    libgralloctypes
    libcamera_metadata    # NDK camera metadata (fillStaticMetadata)
    log
    c++_static
    c++abi_static
)
target_include_directories(vcam_provider_aidl PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_options(vcam_provider_aidl PRIVATE -O2 -Wall)