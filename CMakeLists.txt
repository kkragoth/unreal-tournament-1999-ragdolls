cmake_minimum_required(VERSION 3.7 FATAL_ERROR)
project(unreal-tournament-1999-ragdolls LANGUAGES C CXX)
enable_language(C)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 98)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Where the built RagdollPkg.dll lands. Defaults to <repo>/System/; point
# GAME_SYSTEM_DIR at a retail install's System directory to deploy in one step.
set(GAME_SYSTEM_DIR "" CACHE PATH "Retail UT99 System dir that receives RagdollPkg.dll (optional)")
if(GAME_SYSTEM_DIR)
    set(SYSTEM_DIR ${GAME_SYSTEM_DIR})
else()
    set(SYSTEM_DIR ${PROJECT_SOURCE_DIR}/System)
endif()
file(MAKE_DIRECTORY ${SYSTEM_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${SYSTEM_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${SYSTEM_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

if(WIN32)
    if(MSVC)
        # The prebuilt Core/Engine import libs and the retail engine DLLs are
        # built with wchar_t as a non-native type; match them or LNK2038/LNK4243.
        add_compile_options("/Zc:wchar_t-")
    endif(MSVC)
    add_definitions(-D_UNICODE -DUNICODE)
endif(WIN32)
add_definitions(-DUTGLR_NO_APP_MALLOC)

if(NOT WIN32)
    message(FATAL_ERROR "This project builds UT99 game DLLs; only Windows is supported.")
endif()

# Box3D (thirdparty/box3d) - vendored static C17 physics library used by
# RagdollPkg. See thirdparty/box3d/VENDORED.md for provenance.
add_subdirectory(thirdparty/box3d)
# Box3D forces a static CRT (/MT) for its own directory scope, but the prebuilt
# Core/Engine libs link the multithreaded DLL runtime (/MD). Align box3d with
# them to avoid LNK2038 "RuntimeLibrary" mismatch at link time.
set_target_properties(box3d PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Minimal Unreal Engine 1 (v432/436) SDK subset: headers + prebuilt import libs
# for Core and Engine. The matching Core.dll/Engine.dll ship with the retail game.
add_library(Core INTERFACE)
target_include_directories(Core INTERFACE thirdparty/ut99sdk/Core/Inc)
target_link_libraries(Core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/ut99sdk/Core/Lib/Core.lib)

add_library(Engine INTERFACE)
target_include_directories(Engine INTERFACE thirdparty/ut99sdk/Engine/Inc)
target_link_libraries(Engine INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/ut99sdk/Engine/Lib/Engine.lib)

add_subdirectory(RagdollPkg)
