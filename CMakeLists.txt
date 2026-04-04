cmake_minimum_required(VERSION 3.20)
project(Host)

set(CMAKE_CXX_STANDARD 17)

set(CEF_ROOT "${CMAKE_SOURCE_DIR}/cef")
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CEF_ROOT}/cmake")

find_package(CEF REQUIRED)

add_subdirectory(${CEF_ROOT}/libcef_dll libcef_dll)

add_executable(Host "main.cpp" "simple_app.cpp" "shm/SharedMemoryLayout.h" "shm/FrameBuffer.h" "shm/InputBuffer.h" "osr_handler.h"  "osr_handler.cpp")

target_include_directories(Host PRIVATE ${CEF_ROOT})

target_link_libraries(Host
    libcef_dll_wrapper
    ${CEF_STANDARD_LIBS}
)

set(CEF_BINARY_DIR "$<IF:$<CONFIG:Debug>,${CEF_ROOT}/Debug,${CEF_ROOT}/Release>")

add_custom_command(TARGET Host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CEF_BINARY_DIR}"
        "$<TARGET_FILE_DIR:Host>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CEF_ROOT}/Resources"
        "$<TARGET_FILE_DIR:Host>"
)

SET_EXECUTABLE_TARGET_PROPERTIES(Host)
ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
target_link_libraries(Host libcef_lib)