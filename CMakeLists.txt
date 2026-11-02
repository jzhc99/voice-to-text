cmake_minimum_required(VERSION 3.15)
project(GeminiDictation C)

add_executable(GeminiDictation WIN32 src/main.c src/app.rc)
target_link_libraries(GeminiDictation PRIVATE winmm winhttp user32 gdi32 shell32 advapi32 crypt32 bcrypt)

if(MSVC)
  # The source has non-ASCII string literals
  target_compile_options(GeminiDictation PRIVATE /utf-8 /W3)
  target_compile_definitions(GeminiDictation PRIVATE _CRT_SECURE_NO_WARNINGS)
  set_property(TARGET GeminiDictation PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
else()
  target_compile_options(GeminiDictation PRIVATE -O2 -Wall)
  target_link_options(GeminiDictation PRIVATE -static)
endif()
