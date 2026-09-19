/**
 * @file MD7CoreDefs.cpp
 * @brief Global definitions for the minimal MobileDumper-7 core (libMobileDumperCore.a).
 *
 * This translation unit exists so the FNamePool-reading core (NameArray.cpp /
 * UnrealTypes.cpp) can be linked into another project WITHOUT dragging in the
 * full dumper: Offsets.cpp (which contains the offset *detectors* and pulls in
 * ObjectArray, UnrealObjects, Layouts-detector, KittyMemoryEx and the Zip/Apk
 * utils) is deliberately NOT part of the core library.
 *
 * The globals below are declared in "Engine/OffsetFinder/Offsets.h" and are
 * normally defined in Engine/OffsetFinder/Offsets.cpp. Here they start at
 * zero/null and are populated by the embedding project through the bridge
 * (MobileDumper-7's headers are all that is required to reach them).
 *
 * Built only when CMake is configured with -DMD7_BUILD_CORE=ON.
 * NOT compiled into the normal MobileDumper-7 shared library.
 */

#include "../Engine/OffsetFinder/Offsets.h"
#include "../Settings.h"

/* Global state, declared in Offsets.h, normally defined in Offsets.cpp. */
uintptr_t GObjects = 0x0;
uintptr_t GNames = 0x0;
FLayouts GLayouts = {};
FInGenOffsets GOffsets = {};
FInSDKOffsets GInSDKOffsets = {};
