cmake_minimum_required(VERSION 3.25)

project(levelmate_wasapi_probe VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The LevelMate WASAPI probe can only be built on Windows.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The LevelMate WASAPI probe currently targets Windows x64 only.")
endif()

add_executable(levelmate-wasapi-probe src/main.cpp)

function(configure_levelmate_target target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
    )
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc>
    )
    target_link_libraries(${target} PRIVATE mmdevapi ole32)
endfunction()

configure_levelmate_target(levelmate-wasapi-probe)

include(CTest)
if(BUILD_TESTING)
    add_executable(levelmate-tests tests/main_tests.cpp)
    configure_levelmate_target(levelmate-tests)
    add_test(NAME levelmate-unit-tests COMMAND levelmate-tests)
endif()
