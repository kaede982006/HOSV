cmake_minimum_required(VERSION 3.16)

project(seedance2_cpp
    VERSION 0.1.0
    DESCRIPTION "C++ dynamic library for the Seedance 2 video generation API"
    LANGUAGES CXX)

option(SEEDANCE2_BUILD_EXAMPLES "Build Seedance 2 examples" ON)
option(SEEDANCE2_USE_BUNDLED_DEPS "Build bundled libcurl and mbedTLS from third_party" ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(SEEDANCE2_USE_BUNDLED_DEPS)
    list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
    set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
    set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/mbedtls EXCLUDE_FROM_ALL)

    set(MBEDTLS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/include" CACHE PATH "" FORCE)
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
    set(CURL_CA_BUNDLE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cacert.pem" CACHE STRING "" FORCE)
    set(CURL_CA_PATH "none" CACHE STRING "" FORCE)
    set(HTTP_ONLY ON CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/curl-8.11.1 EXCLUDE_FROM_ALL)
else()
    find_package(CURL REQUIRED)
endif()

add_library(seedance2 SHARED
    src/seedance2.cpp)

add_library(seedance2::seedance2 ALIAS seedance2)

target_compile_features(seedance2 PUBLIC cxx_std_17)
target_compile_definitions(seedance2 PRIVATE SEEDANCE2_BUILDING_LIBRARY)
if(SEEDANCE2_USE_BUNDLED_DEPS)
    target_compile_definitions(seedance2 PRIVATE CURL_STATICLIB)
endif()

target_include_directories(seedance2
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_link_libraries(seedance2 PRIVATE CURL::libcurl)

set_target_properties(seedance2 PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

if(SEEDANCE2_BUILD_EXAMPLES)
    add_executable(seedance2_example examples/text_to_video.cpp)
    target_link_libraries(seedance2_example PRIVATE seedance2::seedance2)
endif()

include(GNUInstallDirs)

install(TARGETS seedance2
    EXPORT seedance2Targets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT seedance2Targets
    NAMESPACE seedance2::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/seedance2)
