cmake_minimum_required(VERSION 3.21)
project(Airdraft LANGUAGES CXX RC)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build Airdraft with the x64 MSVC toolchain")
endif()

add_executable(Airdraft WIN32 main.cpp backend.cpp identity.cpp window_style.cpp app.rc)
target_compile_features(Airdraft PRIVATE cxx_std_20)
target_compile_definitions(Airdraft PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(Airdraft PRIVATE /W4 /WX /utf-8)
set_property(TARGET Airdraft PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_link_libraries(Airdraft PRIVATE advapi32 bcrypt comctl32 crypt32 gdi32 iphlpapi ncrypt ole32
    shell32 user32 windowsapp wtsapi32)
