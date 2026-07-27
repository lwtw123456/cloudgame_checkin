cmake_minimum_required(VERSION 3.15)
project(cloudgame_checkin CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Static runtime linkage
# ---------------------------------------------------------------------------
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    add_link_options(-static-libgcc -static-libstdc++)
endif()

# ---------------------------------------------------------------------------
# Dependencies via find_package (vcpkg or system)
# ---------------------------------------------------------------------------
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)

find_path(HTTPLIB_INCLUDE_DIR
    NAMES httplib.h
    HINTS
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/include
)
if(NOT HTTPLIB_INCLUDE_DIR)
    message(FATAL_ERROR "httplib.h not found. Put it next to CMakeLists.txt or install via vcpkg (cpp-httplib).")
endif()

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(cloudgame_checkin main.cpp)

target_include_directories(cloudgame_checkin PRIVATE
    ${HTTPLIB_INCLUDE_DIR}
	${CMAKE_SOURCE_DIR} 
)

target_compile_definitions(cloudgame_checkin PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)

target_link_libraries(cloudgame_checkin PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
)

# ---------------------------------------------------------------------------
# Platform-specific static linking extras
# ---------------------------------------------------------------------------
if(WIN32)
    target_link_libraries(cloudgame_checkin PRIVATE
        ws2_32
        crypt32
    )
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(cloudgame_checkin PRIVATE Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# MSVC warnings / optimisations
# ---------------------------------------------------------------------------
if(MSVC)
    target_compile_options(cloudgame_checkin PRIVATE /W3 /permissive- /utf-8)
    target_compile_options(cloudgame_checkin PRIVATE /Gy)
    target_compile_options(cloudgame_checkin PRIVATE /GL)
	add_compile_options(/utf-8)
    target_link_options(cloudgame_checkin PRIVATE /LTCG /OPT:REF /OPT:ICF)
else()
    target_compile_options(cloudgame_checkin PRIVATE -Wall -Wextra -O2)
    target_compile_options(cloudgame_checkin PRIVATE -ffunction-sections -fdata-sections)
    if(APPLE)
        target_link_options(cloudgame_checkin PRIVATE -Wl,-dead_strip)
    else()
        target_link_options(cloudgame_checkin PRIVATE -Wl,--gc-sections)
    endif()
endif()

cmake -S . -B build ^
  -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static ^
  -G "NMake Makefiles" ^
  -DCMAKE_BUILD_TYPE=Release