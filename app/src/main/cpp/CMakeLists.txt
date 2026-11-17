cmake_minimum_required(VERSION 3.24)
project(OuterSSHClient LANGUAGES C)

set(OUTERSSH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../OuterSSH/OuterSSH")
add_subdirectory("${OUTERSSH_DIR}" outerssh)

add_library(client SHARED client.c)
target_compile_options(client PRIVATE -Wall -Wextra -Werror)
target_link_libraries(client PRIVATE OuterSSH android log)
