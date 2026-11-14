cmake_minimum_required(VERSION 3.20)
project(picocalc_ssh_tests C)
set(CMAKE_C_STANDARD 11)
enable_testing()
add_executable(core_tests core_tests.c ../src/model.c ../src/terminal.c)
target_include_directories(core_tests PRIVATE ../src)
target_compile_options(core_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG
    -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(core_tests PRIVATE -fsanitize=address,undefined)
add_test(NAME core COMMAND core_tests)
add_executable(ui_tests ui_tests.c ../src/app.c ../src/model.c ../src/terminal.c)
target_include_directories(ui_tests PRIVATE include ../src)
target_compile_options(ui_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG
    -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(ui_tests PRIVATE -fsanitize=address,undefined)
target_compile_definitions(ui_tests PRIVATE APP_VERSION="9.8.7-test")
add_test(NAME ui COMMAND ui_tests)
add_executable(network_tests network_tests.c ../src/network.c ../src/model.c)
target_include_directories(network_tests PRIVATE include ../src)
target_compile_options(network_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG
    -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(network_tests PRIVATE -fsanitize=address,undefined)
add_test(NAME network COMMAND network_tests)
file(READ "${CMAKE_CURRENT_LIST_DIR}/../third_party/fatfs/ffconf.h" fatfs_config)
string(REGEX REPLACE "#define FF_USE_MKFS[ \t]+0" "#define FF_USE_MKFS 1"
    fatfs_config "${fatfs_config}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/test-ffconf.h" "${fatfs_config}")
add_executable(storage_tests storage_tests.c ../src/storage.c ../src/model.c
    ../third_party/fatfs/ff.c ../third_party/fatfs/ffunicode.c)
target_include_directories(storage_tests PRIVATE ../src ../third_party/fatfs)
target_compile_options(storage_tests PRIVATE -include "${CMAKE_CURRENT_BINARY_DIR}/test-ffconf.h"
    -UNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(storage_tests PRIVATE -fsanitize=address,undefined)
add_test(NAME storage COMMAND storage_tests)
include(../tools/crypto.cmake)
add_executable(ssh_integration ssh_integration.c host_platform.c
    ../src/ssh_client.c ../src/model.c)
target_include_directories(ssh_integration PRIVATE ../src)
target_compile_definitions(ssh_integration PRIVATE SSH_HOST_TEST)
target_compile_options(ssh_integration PRIVATE -Wall -Wextra -Werror -UNDEBUG
    -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(ssh_integration PRIVATE -fsanitize=address,undefined)
target_link_libraries(ssh_integration PRIVATE ssh_crypto)
