cmake_minimum_required(VERSION 3.20)
project(OllvmPass VERSION 1.0.0 LANGUAGES CXX)

# ── Require C++17 ──────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Find LLVM ──────────────────────────────────────────────────────────────────
find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake from ${LLVM_DIR}")

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(HandleLLVMOptions)
add_definitions(${LLVM_DEFINITIONS})

# ── Sources ────────────────────────────────────────────────────────────────────
set(PASS_SOURCES
    src/ObfuscationPlugin.cpp
    src/ControlFlowFlattening.cpp
    src/BogusControlFlow.cpp
    src/InstructionSubstitution.cpp
    src/StringEncryption.cpp
    src/ConstantEncryption.cpp
    src/IndirectBranching.cpp
)

# ── Build as a shared library (LLVM plugin) ────────────────────────────────────
add_library(OllvmPass MODULE ${PASS_SOURCES})

target_include_directories(OllvmPass PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Link against the required LLVM components
llvm_map_components_to_libnames(LLVM_LIBS
    Core
    Support
    IRReader
    Analysis
    TransformUtils
    Passes
)

target_link_libraries(OllvmPass PRIVATE ${LLVM_LIBS})

# ── MSVC-specific settings ─────────────────────────────────────────────────────
if(MSVC)
    target_compile_options(OllvmPass PRIVATE
        /W3          # Warning level 3
        /WX-         # Don't treat warnings as errors
        /GR-         # Disable RTTI (matches LLVM build)
        /EHs-        # Disable synchronous exception handling
        /EHc-        # Disable C++ EH
        /D_CRT_SECURE_NO_WARNINGS
        /DWIN32
        /D_WINDOWS
        /D_UNICODE
        /DUNICODE
    )
    # Export all symbols on Windows for the plugin entry point
    set_target_properties(OllvmPass PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
else()
    target_compile_options(OllvmPass PRIVATE
        -fno-rtti
        -Wall
        -Wextra
        -Wno-unused-parameter
    )
endif()

# ── Output naming ──────────────────────────────────────────────────────────────
set_target_properties(OllvmPass PROPERTIES
    OUTPUT_NAME "OllvmPass"
    PREFIX ""
)

# ── Install ────────────────────────────────────────────────────────────────────
install(TARGETS OllvmPass DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)

# ── Optional: standalone test driver ──────────────────────────────────────────
option(BUILD_TEST "Build the sample Windows kernel driver test" OFF)
if(BUILD_TEST)
    add_subdirectory(test)
endif()
