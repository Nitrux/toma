cmake_minimum_required(VERSION 3.21)

project(toma VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Toma supports Linux builds only.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CheckCXXCompilerFlag)
include(GNUInstallDirs)

string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" TOMA_HOST_PROCESSOR)
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" TOMA_TARGET_PROCESSOR)

if(NOT CMAKE_CROSSCOMPILING
   AND TOMA_HOST_PROCESSOR MATCHES "^(x86_64|amd64)$"
   AND TOMA_TARGET_PROCESSOR MATCHES "^(x86_64|amd64)$")
    check_cxx_compiler_flag("-march=x86-64-v3" TOMA_HAS_X86_64_V3)
    if(TOMA_HAS_X86_64_V3)
        add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-march=x86-64-v3>)
    endif()
endif()

add_executable(toma src/main.cpp src/config.cpp src/config.h)
target_compile_features(toma PRIVATE cxx_std_23)
set_target_properties(toma PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

install(TARGETS toma
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    IMMEDIATE @ONLY)

add_custom_target(uninstall
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake")
