cmake_minimum_required(VERSION 3.20)
project(xzero C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

# Warnings
if(MSVC)
    add_compile_options(/W4 /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    add_compile_definitions(_POSIX_C_SOURCE=200809L _DEFAULT_SOURCE _GNU_SOURCE)
endif()

# Ensure PATH_MAX etc available on POSIX strict builds
if(UNIX)
    add_compile_definitions(_POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
endif()

# Options
option(XZERO_USE_SYSTEM_CURL "Use system curl if found" ON)
option(BUILD_TESTING "Build tests" ON)
# Dependencies force BUILD_TESTING off/on below; remember the user's choice.
set(XZERO_BUILD_TESTING ${BUILD_TESTING})

include(FetchContent)

# Try system curl first
set(CURL_FOUND FALSE)
if(XZERO_USE_SYSTEM_CURL)
    find_package(CURL QUIET)
    if(CURL_FOUND)
        message(STATUS "Using system CURL: ${CURL_LIBRARIES}")
    endif()
endif()

if(NOT CURL_FOUND)
    message(STATUS "Fetching libcurl via FetchContent...")
    FetchContent_Declare(
        curl
        URL https://curl.se/download/curl-8.11.0.tar.gz
        URL_HASH SHA256=264537d90e58d2b09dddc50944baf3c38e7089151c8986715e2aaeaaf2b8118f
    )
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(CURL_ENABLE_SSL ON CACHE BOOL "" FORCE)
    if(WIN32)
        set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
        set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
    endif()
    # This will be slow on first configure - allow fallback if download fails
    set(FETCHCONTENT_QUIET OFF)
    FetchContent_MakeAvailable(curl)
# curl disables BUILD_TESTING globally - restore the user's choice
set(BUILD_TESTING ${XZERO_BUILD_TESTING} CACHE BOOL "" FORCE)
endif()

# cJSON via FetchContent - force static, no own tests
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_UTILS OFF CACHE BOOL "" FORCE)
set(CJSON_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    cJSON
    URL https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.18.tar.gz
    URL_HASH SHA256=3aa806844a03442c00769b83e99970be70fbef03735ff898f4811dd03b9f5ee5
)
FetchContent_MakeAvailable(cJSON)
set(BUILD_TESTING ${XZERO_BUILD_TESTING} CACHE BOOL "" FORCE)

# Sources
set(XZERO_SOURCES
    src/main.c
    src/util.c
    src/config.c
    src/prompt.c
    src/http_curl.c
    src/openai.c
    src/stream.c
    src/session.c
    src/agent.c
    src/repl.c
    src/tools/registry.c
    src/tools/read.c
    src/tools/write.c
    src/tools/edit.c
    src/tools/bash.c
    src/tools/glob.c
    src/tools/grep.c
    src/wallpaper.c
)

add_executable(xzero ${XZERO_SOURCES})

target_include_directories(xzero PRIVATE src)

if(CURL_FOUND)
    target_include_directories(xzero PRIVATE ${CURL_INCLUDE_DIRS})
    target_link_libraries(xzero PRIVATE ${CURL_LIBRARIES})
else()
    target_link_libraries(xzero PRIVATE libcurl)
    target_include_directories(xzero PRIVATE ${curl_SOURCE_DIR}/include)
endif()

target_link_libraries(xzero PRIVATE cjson)
target_include_directories(xzero PRIVATE ${cjson_SOURCE_DIR})

# Platform libs
if(WIN32)
    target_link_libraries(xzero PRIVATE ws2_32 wsock32 winhttp crypt32 user32)
endif()

if(UNIX)
    target_link_libraries(xzero PRIVATE m pthread)
endif()

# Tests
enable_testing()
if(XZERO_BUILD_TESTING)
    add_executable(test_config tests/test_config.c src/config.c src/util.c)
    target_include_directories(test_config PRIVATE src ${cjson_SOURCE_DIR})
    target_link_libraries(test_config PRIVATE cjson)
    if(WIN32)
        target_link_libraries(test_config PRIVATE ws2_32)
    else()
        target_link_libraries(test_config PRIVATE m)
    endif()
    add_test(NAME test_config COMMAND test_config)

    add_executable(test_stream tests/test_stream.c src/stream.c src/util.c src/http_curl.c)
    target_include_directories(test_stream PRIVATE src ${cjson_SOURCE_DIR})
    if(CURL_FOUND)
        target_include_directories(test_stream PRIVATE ${CURL_INCLUDE_DIRS})
        target_link_libraries(test_stream PRIVATE cjson ${CURL_LIBRARIES})
    else()
        target_include_directories(test_stream PRIVATE ${curl_SOURCE_DIR}/include)
        target_link_libraries(test_stream PRIVATE cjson libcurl)
    endif()
    if(WIN32)
        target_link_libraries(test_stream PRIVATE ws2_32 wsock32 winhttp crypt32)
    else()
        target_link_libraries(test_stream PRIVATE m pthread)
    endif()
    add_test(NAME test_stream COMMAND test_stream)

    add_executable(test_session tests/test_session.c src/session.c src/util.c)
    target_include_directories(test_session PRIVATE src ${cjson_SOURCE_DIR})
    target_link_libraries(test_session PRIVATE cjson)
    if(WIN32)
        target_link_libraries(test_session PRIVATE ws2_32)
    else()
        target_link_libraries(test_session PRIVATE m)
    endif()
    add_test(NAME test_session COMMAND test_session)

    add_executable(test_wallpaper tests/test_wallpaper.c src/wallpaper.c src/util.c src/http_curl.c)
    target_include_directories(test_wallpaper PRIVATE src ${cjson_SOURCE_DIR})
    if(CURL_FOUND)
        target_include_directories(test_wallpaper PRIVATE ${CURL_INCLUDE_DIRS})
        target_link_libraries(test_wallpaper PRIVATE ${CURL_LIBRARIES})
    else()
        target_include_directories(test_wallpaper PRIVATE ${curl_SOURCE_DIR}/include)
        target_link_libraries(test_wallpaper PRIVATE libcurl)
    endif()
    if(WIN32)
        target_link_libraries(test_wallpaper PRIVATE ws2_32 wsock32 winhttp crypt32 user32)
    else()
        target_link_libraries(test_wallpaper PRIVATE m pthread)
    endif()
    add_test(NAME test_wallpaper COMMAND test_wallpaper)

    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME test_integration
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_test.py $<TARGET_FILE:xzero>)
        set_tests_properties(test_integration PROPERTIES TIMEOUT 180)
    endif()
endif()

message(STATUS "xzero configured: C_STANDARD=${CMAKE_C_STANDARD} CURL_FOUND=${CURL_FOUND}")
