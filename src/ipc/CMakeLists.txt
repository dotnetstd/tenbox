set(TENBOX_IPC_SOURCES
    ${CMAKE_SOURCE_DIR}/src/ipc/protocol_v1.cpp
)

if(NOT WIN32)
    list(APPEND TENBOX_IPC_SOURCES
        ${CMAKE_SOURCE_DIR}/src/ipc/unix_socket.cpp
    )
endif()

add_library(tenbox_ipc STATIC ${TENBOX_IPC_SOURCES})

target_include_directories(tenbox_ipc
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}
)
