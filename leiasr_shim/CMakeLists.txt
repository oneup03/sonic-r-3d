# leiasr_shim — MSVC-built C-ABI bridge to the LeiaSR OpenGL weaver.
#
# Built SEPARATELY from the game, which is a MinGW/Make build. See
# leiasr_shim/README.md for why, and the header comment in leiasr_shim.cpp for
# the ABI details.
#
#   cmake -S leiasr_shim -B leiasr_shim/build -A x64
#   cmake --build leiasr_shim/build --config Release
#
# Produces build/Release/leiasr_shim.dll, which goes next to SONICR.EXE. If it
# is not built or not deployed, the game still runs — LoadLibrary fails and
# LeiaSR mode falls back to plain Side-by-Side.

cmake_minimum_required(VERSION 3.15)
project(leiasr_shim CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT. The point of this DLL is that it drops in next to the game and
# either works or is silently absent — so it must not add a "please install the
# VC++ Redistributable" failure mode of its own. With /MT the only non-system
# imports left are the delay-loaded SR DLLs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
cmake_policy(SET CMP0091 NEW)

# SR-lib is a git submodule (libs/SR-lib, api_expansion branch). Its CMake
# package locates the nested per-architecture SDK tree itself — it globs
# SR-SDK-* for the version directory so a version bump doesn't break us, and
# picks x64/x32 from the pointer size.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../libs/SR-lib" srlib)

if(NOT SRLIB_FOUND)
    message(FATAL_ERROR
        "SR-lib not found. The submodule is probably not checked out:\n"
        "    git submodule update --init --recursive\n"
        "SR-lib is a public repo, so this needs no credentials.")
endif()

add_library(leiasr_shim SHARED leiasr_shim.cpp)

# SRLib::SR is the wrapper (SR.cpp), not just the import libs — it owns the
# SRContext lifecycle, the create->weaver->initialize ordering, the per-backend
# DLL probing and the exception containment. Linking SRLib::SDK instead would
# mean reimplementing all of that here.
target_link_libraries(leiasr_shim PRIVATE SRLib::SR)

# /DELAYLOAD for every SR-touched DLL, so this DLL loads on machines without the
# SR runtime installed instead of failing with a loader popup before any of our
# code runs. srlib_apply_delayload owns the list — including
# opencv_world343.dll, which the SR import libs pull in via inlined cv::String
# destructors even though we write no OpenCV code, and which is the DLL such
# popups almost always name.
#
# It is deliberately the complete list rather than one tailored to this
# consumer, so expect LNK4199 ("no imports found from ...") for the DLLs we
# don't touch. That warning is the intended outcome, not a misconfiguration.
srlib_apply_delayload(leiasr_shim)

if(MSVC)
    # /EHa so catch(...) also catches SEH. The skill warns against this
    # ABI-wide, because it widens what catch(...) means for every function in
    # the translation unit — but this TU is nothing BUT the SR shim, so there is
    # no non-SR code in scope for it to affect. It is the outer belt behind
    # SR-lib's LoadLibraryW preflight: the preflight fails fast on the
    # no-runtime path, and /EHa catches anything that still escapes as an SEH
    # (e.g. a partial install where one DLL resolves and a sibling doesn't).
    target_compile_options(leiasr_shim PRIVATE /EHa)
    target_link_libraries(leiasr_shim PRIVATE delayimp)
endif()

# No import lib / exp needed by the consumer — the game resolves every export
# through GetProcAddress, never at link time.
set_target_properties(leiasr_shim PROPERTIES
    PREFIX ""
    OUTPUT_NAME "leiasr_shim")
