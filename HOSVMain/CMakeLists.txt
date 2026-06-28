cmake_minimum_required(VERSION 3.16)

project(HOSV
    VERSION 0.1.0
    DESCRIPTION "Native GUI application for AI video generation using BytePlus ModelArk"
    LANGUAGES CXX)

option(HOSV_ENABLE_BYTEPLUS_ARK "Build the official BytePlus ModelArk Go bridge provider" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(HOSV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(HOSV_API_DIR "${HOSV_ROOT}/HOSVApi")
set(HOSV_ARKBRIDGE_DIR "${HOSV_ROOT}/tools/arkbridge")
set(HOSV_LOGO "${HOSV_ROOT}/logo.png")

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
add_subdirectory("${HOSV_API_DIR}/third_party/mbedtls" "${CMAKE_BINARY_DIR}/third_party/mbedtls" EXCLUDE_FROM_ALL)

set(MBEDTLS_INCLUDE_DIR "${HOSV_API_DIR}/third_party/mbedtls/include" CACHE PATH "" FORCE)
set(MBEDTLS_LIBRARY mbedtls CACHE STRING "" FORCE)
set(MBEDX509_LIBRARY mbedx509 CACHE STRING "" FORCE)
set(MBEDCRYPTO_LIBRARY mbedcrypto CACHE STRING "" FORCE)

set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(CURL_ENABLE_EXPORT_TARGET OFF CACHE BOOL "" FORCE)
set(CURL_USE_PKGCONFIG OFF CACHE BOOL "" FORCE)
set(CURL_USE_MBEDTLS ON CACHE BOOL "" FORCE)
set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH OFF CACHE BOOL "" FORCE)
set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
set(CURL_CA_BUNDLE "${HOSV_API_DIR}/third_party/cacert.pem" CACHE STRING "" FORCE)
set(CURL_CA_PATH "none" CACHE STRING "" FORCE)
set(HTTP_ONLY ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${HOSV_API_DIR}/third_party/curl-8.11.1" "${CMAKE_BINARY_DIR}/third_party/curl" EXCLUDE_FROM_ALL)

add_executable(hosv
    src/main.cpp
    src/SeedanceService.cpp
    src/JobController.cpp
    src/providers/BridgeProcessRunner.cpp
    src/providers/BytePlusArkVideoProvider.cpp)

target_include_directories(hosv PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${HOSV_API_DIR}/include"
    "${HOSV_API_DIR}/third_party"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

target_compile_definitions(hosv PRIVATE
    CURL_STATICLIB
    HOSV_VERSION="${PROJECT_VERSION}")

target_link_libraries(hosv PRIVATE
    CURL::libcurl
    dl
    pthread)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(hosv PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(hosv PRIVATE -static-libgcc -static-libstdc++)
endif()

if(HOSV_ENABLE_BYTEPLUS_ARK)
    find_program(GO_EXECUTABLE go)
    if(NOT GO_EXECUTABLE)
        message(FATAL_ERROR "Go is required to build arkbridge. Install Go or set HOSV_ENABLE_BYTEPLUS_ARK=OFF.")
    endif()

    set(ARKBRIDGE_OUTPUT "${CMAKE_BINARY_DIR}/arkbridge")
    add_custom_command(
        OUTPUT "${ARKBRIDGE_OUTPUT}"
        COMMAND "${GO_EXECUTABLE}" build -o "${ARKBRIDGE_OUTPUT}" ./cmd/arkbridge
        WORKING_DIRECTORY "${HOSV_ARKBRIDGE_DIR}"
        DEPENDS
            "${HOSV_ARKBRIDGE_DIR}/go.mod"
            "${HOSV_ARKBRIDGE_DIR}/cmd/arkbridge/main.go"
            "${HOSV_ARKBRIDGE_DIR}/internal/config/config.go"
            "${HOSV_ARKBRIDGE_DIR}/internal/byteplus/client.go"
            "${HOSV_ARKBRIDGE_DIR}/internal/byteplus/tasks.go"
            "${HOSV_ARKBRIDGE_DIR}/internal/byteplus/status.go"
            "${HOSV_ARKBRIDGE_DIR}/internal/bridge/protocol.go"
            "${HOSV_ARKBRIDGE_DIR}/internal/logging/redact.go"
        COMMENT "Build official BytePlus ModelArk arkbridge")
    add_custom_target(arkbridge_target ALL DEPENDS "${ARKBRIDGE_OUTPUT}")
    add_dependencies(hosv arkbridge_target)
    add_custom_command(
        TARGET hosv
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ARKBRIDGE_OUTPUT}"
            "$<TARGET_FILE_DIR:hosv>/arkbridge"
        COMMENT "Copy arkbridge next to hosv executable")
endif()

install(TARGETS hosv RUNTIME DESTINATION .)
install(FILES "${HOSV_LOGO}" DESTINATION . OPTIONAL)

add_custom_command(
    TARGET hosv
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${HOSV_LOGO}"
        "$<TARGET_FILE_DIR:hosv>/logo.png"
    COMMENT "Copy logo.png next to hosv executable")
