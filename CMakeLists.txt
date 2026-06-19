cmake_minimum_required(VERSION 3.25)

project(levelmate_wasapi_probe VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The LevelMate WASAPI probe can only be built on Windows.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The LevelMate WASAPI probe currently targets Windows x64 only.")
endif()

add_executable(levelmate-wasapi-probe src/main.cpp)
target_compile_features(levelmate-wasapi-probe PRIVATE cxx_std_20)
target_compile_definitions(levelmate-wasapi-probe PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)
target_compile_options(levelmate-wasapi-probe PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc>
)
target_link_libraries(levelmate-wasapi-probe PRIVATE mmdevapi ole32)
