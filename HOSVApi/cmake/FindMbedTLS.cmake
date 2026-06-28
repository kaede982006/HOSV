set(MBEDTLS_FOUND TRUE)
set(MBEDTLS_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../third_party/mbedtls/include")
set(MBEDTLS_LIBRARY_DIRS "")
set(MBEDTLS_PC_REQUIRES "")

if(TARGET mbedtls AND TARGET mbedx509 AND TARGET mbedcrypto)
    set(MBEDTLS_LIBRARIES mbedtls mbedx509 mbedcrypto)
else()
    set(MBEDTLS_FOUND FALSE)
endif()

