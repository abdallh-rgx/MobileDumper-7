cmake_minimum_required(VERSION 3.22)

project(MobileDumper-7 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(DISABLED_WARNINGS
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-missing-field-initializers
    -Wno-unknown-warning-option
)

if(NOT DEFINED DUMPER_ROOT)
    set(DUMPER_ROOT "${CMAKE_SOURCE_DIR}")
endif()

option(DUMPER_BUILD_EXECUTABLE "Build as a standalone executable instead of a shared library" OFF)

# ── Platform detection ────────────────────────────────────────────────────────

if(CMAKE_SYSTEM_NAME STREQUAL "Android" OR ANDROID)
    set(PLATFORM_ANDROID TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(PLATFORM_IOS TRUE)
    enable_language(OBJCXX)
else()
    message(FATAL_ERROR "Unsupported platform: '${CMAKE_SYSTEM_NAME}'. Set -DCMAKE_SYSTEM_NAME=Android or iOS.")
endif()

# ── Third-party ───────────────────────────────────────────────────────────────

set(FMT_PATH "${DUMPER_ROOT}/libs/fmt")

if(PLATFORM_ANDROID)
    set(KITTYMEM_PATH "${DUMPER_ROOT}/libs/KittyMemoryEx/KittyMemoryEx")
elseif(PLATFORM_IOS)
    set(KITTYMEM_PATH "${DUMPER_ROOT}/libs/KittyMemory/KittyMemory")
endif()

file(GLOB KITTYMEM_SRC "${KITTYMEM_PATH}/*.cpp")

# ── Embedding core ────────────────────────────────────────────────────────────
#
# Builds ONLY the FNamePool-reading core as a static library (libMobileDumperCore.a)
# for embedding into other projects (e.g. OwenGameServer).
#
# What is included:  MD7CoreDefs.cpp (globals), NameArray.cpp, UnrealTypes.cpp
# What is excluded:  main_android.cpp (the __attribute__((constructor)) entry point),
#                    DumperMain.cpp (RunDump), Generator/* (SDK/zip/idmap/dumpspace/
#                    mappings generation), UEAnalyzerKitty, MemoryAndroid.cpp
#                    (KittyMemoryEx), ObjectArray, UnrealObjects, UnrealContainers,
#                    Layouts.cpp / Offsets.cpp (layout & offset detectors), Utils.cpp,
#                    Logger.cpp, Zip/Json/Compression, all iOS sources.
#
# Result: an .a with NO constructor, NO dumper entry point and NO KittyMemoryEx.
option(MD7_BUILD_CORE "Build ONLY the minimal FNamePool-reading core (libMobileDumperCore.a) for embedding into other projects" OFF)

if(MD7_BUILD_CORE)
    if(NOT (CMAKE_SYSTEM_NAME STREQUAL "Android" OR ANDROID))
        message(FATAL_ERROR "MD7_BUILD_CORE requires an Android toolchain (-DCMAKE_SYSTEM_NAME=Android -DANDROID_ABI=arm64-v8a).")
    endif()

    add_library(MobileDumperCore STATIC
        "${DUMPER_ROOT}/Dumper/Core/MD7CoreDefs.cpp"
        "${DUMPER_ROOT}/Dumper/Engine/Unreal/NameArray.cpp"
        "${DUMPER_ROOT}/Dumper/Engine/Unreal/UnrealTypes.cpp"
    )

    target_include_directories(MobileDumperCore PUBLIC
        "${DUMPER_ROOT}"          # consumers include as "Dumper/Memory/IMemory.h" etc.
        "${DUMPER_ROOT}/Dumper"   # internal quote-relative includes
        "${FMT_PATH}/include"     # NameArray.cpp -> Utils/Logger.h -> <fmt/format.h> (header-only, FMT_HEADER_ONLY: no fmt code is emitted)
    )

    target_compile_definitions(MobileDumperCore PRIVATE
        FMT_HEADER_ONLY
        kNO_KEYSTONE
    )

    target_compile_options(MobileDumperCore PRIVATE
        -fexceptions
        -frtti
        -fvisibility=hidden
        -fvisibility-inlines-hidden
        -ffunction-sections
        -fdata-sections
        ${DISABLED_WARNINGS}
        $<$<CONFIG:Debug>:-O0 -g3 -fno-omit-frame-pointer>
        $<$<CONFIG:Release>:-O2 -g0 -fomit-frame-pointer>
    )

    set_target_properties(MobileDumperCore PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON
    )

    # No target_link_libraries on purpose: the core has zero link dependencies
    # (no KittyMemoryEx, no android/log, no fmt binaries, no zlib).

    message(STATUS "==============================================")
    message(STATUS "MobileDumper-7 CORE (embedding mode)")
    message(STATUS "Output: libMobileDumperCore.a")
    message(STATUS "Sources: MD7CoreDefs.cpp + NameArray.cpp + UnrealTypes.cpp")
    message(STATUS "Excluded: main_android.cpp (constructor), DumperMain (RunDump),")
    message(STATUS "Generator/*, UEAnalyzerKitty, MemoryAndroid + KittyMemoryEx,")
    message(STATUS "ObjectArray, UnrealObjects, UnrealContainers, detectors, Utils.cpp,")
    message(STATUS "Logger.cpp, Zip/Json/Dumpspace/iOS sources")
    message(STATUS "==============================================")
    return()
endif()


# ── Dumper sources ────────────────────────────────────────────────────────────

file(GLOB_RECURSE DUMPER_SRC
    "${DUMPER_ROOT}/Dumper/*.cpp"
    "${DUMPER_ROOT}/Dumper/*.mm"
    "${DUMPER_ROOT}/Dumper/*.c"
)

if(PLATFORM_ANDROID)
    list(FILTER DUMPER_SRC EXCLUDE REGEX ".*MemoryiOS.*")
    list(FILTER DUMPER_SRC EXCLUDE REGEX ".*ProfileiOS.*")
    list(FILTER DUMPER_SRC EXCLUDE REGEX ".*/main_ios\\.mm$")
    list(FILTER DUMPER_SRC EXCLUDE REGEX ".*/UI/iOS/.*")
elseif(PLATFORM_IOS)
    list(FILTER DUMPER_SRC EXCLUDE REGEX ".*MemoryAndroid.*")
    list(FILTER DUMPER_SRC EXCLUDE REGEX ".*ProfileAndroid.*")
    list(FILTER DUMPER_SRC EXCLUDE REGEX ".*/main_android\\.cpp$")
endif()

# ── Target ────────────────────────────────────────────────────────────────────

if(DUMPER_BUILD_EXECUTABLE)
    add_executable(MobileDumper-7 ${DUMPER_SRC} ${KITTYMEM_SRC})
else()
    add_library(MobileDumper-7 SHARED ${DUMPER_SRC} ${KITTYMEM_SRC})
endif()

target_include_directories(MobileDumper-7 PRIVATE
    ${KITTYMEM_PATH}/..
    ${FMT_PATH}/include
)

target_compile_definitions(MobileDumper-7 PRIVATE
    FMT_HEADER_ONLY
    kNO_KEYSTONE
    $<$<BOOL:${DUMPER_BUILD_EXECUTABLE}>:DUMPER_BUILD_EXECUTABLE>
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
)

target_compile_options(MobileDumper-7 PRIVATE
    -fexceptions
    ${DISABLED_WARNINGS}
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    $<$<BOOL:${PLATFORM_IOS}>:-fobjc-arc>
    $<$<CONFIG:Debug>:-O0 -g3 -fno-omit-frame-pointer>
    $<$<CONFIG:Release>:-O3 -g0 -fomit-frame-pointer>
)

if(PLATFORM_ANDROID)
    target_link_options(MobileDumper-7 PRIVATE
        $<$<CONFIG:Release>:-s>
    )
endif()

if(PLATFORM_IOS AND CMAKE_BUILD_TYPE STREQUAL "Release")
    add_custom_command(TARGET MobileDumper-7 POST_BUILD
        COMMAND xcrun strip -x $<TARGET_FILE:MobileDumper-7>
        VERBATIM
    )
endif()

if(PLATFORM_ANDROID)
    target_link_libraries(MobileDumper-7 PRIVATE android log)
elseif(PLATFORM_IOS)
    target_link_libraries(MobileDumper-7 PRIVATE "-framework Foundation -framework UIKit -framework CoreGraphics -framework QuartzCore")
    set_target_properties(MobileDumper-7 PROPERTIES
        XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "14.0"
    )
endif()
