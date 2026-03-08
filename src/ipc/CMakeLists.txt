set(TENBOX_IPC_SOURCES
    ${CMAKE_SOURCE_DIR}/src/ipc/protocol_v1.cpp
)

if(WIN32)
    list(APPEND TENBOX_IPC_SOURCES
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/shared_framebuffer_win32.cpp
    )
else()
    list(APPEND TENBOX_IPC_SOURCES
        ${CMAKE_SOURCE_DIR}/src/ipc/unix_socket.cpp
        ${CMAKE_SOURCE_DIR}/src/ipc/shared_framebuffer_posix.cpp
    )
endif()

add_library(tenbox_ipc STATIC ${TENBOX_IPC_SOURCES})

target_include_directories(tenbox_ipc
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}
)
