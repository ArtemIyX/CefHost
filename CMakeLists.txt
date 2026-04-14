cmake_minimum_required(VERSION 3.20)
project(Host)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CEF_ROOT "${CMAKE_SOURCE_DIR}/cef")
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CEF_ROOT}/cmake")

find_package(CEF REQUIRED)

add_subdirectory(${CEF_ROOT}/libcef_dll libcef_dll)

set(HOST_SOURCES
    src/main.cpp
    src/cef_browser_app.cpp
    src/osr_handler.cpp
    src/host_runtime_config.cpp
)

set(HOST_HEADERS
    include/cef_browser_app.h
    include/osr_handler.h
    include/D3d11device.h
    include/host_runtime_config.h
    include/shm/SharedMemoryLayout.h
    include/shm/SharedFrameBuffer.h
    include/shm/SharedInputBuffer.h
    include/shm/SharedControlBuffer.h
    include/shm/SharedConsoleBuffer.h
)

add_executable(Host ${HOST_SOURCES} ${HOST_HEADERS})

target_include_directories(Host PRIVATE ${CEF_ROOT} ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(Host
    libcef_dll_wrapper
    ${CEF_STANDARD_LIBS}
    d3d11.lib
    dxgi.lib
    dxguid.lib
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
