cmake_minimum_required(VERSION 3.21)
project(ExrView VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(EXRVIEW_BUILD_VIEWER   "Build ứng dụng xem ảnh (Qt6)" ON)
option(EXRVIEW_BUILD_SHELLEXT "Build DLL thumbnail/preview cho Windows Explorer" ${WIN32})
option(EXRVIEW_BUILD_TESTS    "Build công cụ test dòng lệnh" ON)

if(MSVC)
    add_compile_options(/utf-8 /W3 /MP)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

# ---------- Thư viện bên thứ ba ----------
find_package(OpenEXR CONFIG REQUIRED)

find_package(OpenColorIO CONFIG QUIET)
if(NOT TARGET OpenColorIO::OpenColorIO)
    # Một số bản phân phối Linux không có CMake config cho OCIO
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(OCIO REQUIRED IMPORTED_TARGET OpenColorIO)
    add_library(OpenColorIO::OpenColorIO ALIAS PkgConfig::OCIO)
endif()

# ---------- Lõi dùng chung ----------
add_library(exrcore STATIC
    core/include/exrcore.h
    core/src/ExrFile.cpp
    core/src/ColorPipeline.cpp
    core/src/FallbackConfig.h)
target_include_directories(exrcore PUBLIC core/include)
target_link_libraries(exrcore PRIVATE OpenEXR::OpenEXR OpenColorIO::OpenColorIO)
set_target_properties(exrcore PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(EXRVIEW_BUILD_TESTS)
    add_executable(exrinfo tools/exrinfo.cpp)
    target_link_libraries(exrinfo PRIVATE exrcore)
    install(TARGETS exrinfo RUNTIME DESTINATION .)
endif()

if(EXRVIEW_BUILD_VIEWER)
    add_subdirectory(viewer)
endif()

if(EXRVIEW_BUILD_SHELLEXT)
    if(NOT WIN32)
        message(FATAL_ERROR "Shell extension chỉ build được trên Windows")
    endif()
    add_subdirectory(shellext)
endif()

# Kèm VC++ runtime (msvcp140/vcruntime140) cạnh exe/dll: DLL preview chạy trong
# explorer/prevhost nên không được giả định máy đã cài VC++ Redistributable.
if(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
    set(CMAKE_INSTALL_UCRT_LIBRARIES OFF)
    include(InstallRequiredSystemLibraries)
endif()
