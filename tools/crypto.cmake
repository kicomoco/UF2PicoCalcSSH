set(WOLFSSL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.deps/wolfssl")
set(WOLFSSH_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.deps/wolfssh")
set(CRYPTO_SOURCES
    aes asn coding curve25519 dh ecc ed25519 fe_low_mem ge_low_mem
    hash hmac logging memory random rsa sha sha256 sha512 sp_int wc_port
    wolfmath signature)
set(CRYPTO_FILES "")
foreach(name IN LISTS CRYPTO_SOURCES)
    list(APPEND CRYPTO_FILES "${WOLFSSL_ROOT}/wolfcrypt/src/${name}.c")
endforeach()
add_library(ssh_crypto STATIC ${CRYPTO_FILES}
    "${WOLFSSH_ROOT}/src/ssh.c" "${WOLFSSH_ROOT}/src/internal.c"
    "${WOLFSSH_ROOT}/src/ossh.c"
    "${WOLFSSH_ROOT}/src/io.c" "${WOLFSSH_ROOT}/src/port.c"
    "${WOLFSSH_ROOT}/src/log.c")
target_include_directories(ssh_crypto PUBLIC
    "${WOLFSSL_ROOT}" "${WOLFSSH_ROOT}" "${CMAKE_CURRENT_LIST_DIR}/../src")
target_compile_definitions(ssh_crypto PUBLIC WOLFSSL_USER_SETTINGS)
target_compile_options(ssh_crypto PRIVATE -Wno-unused-parameter)
