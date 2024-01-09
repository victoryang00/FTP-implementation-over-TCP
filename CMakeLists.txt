cmake_minimum_required(VERSION 2.8)

project(ftp CXX)

set(CMAKE_C_STANDARD 11)

set(PROGRAMS
  dataServer
  remoteClient
)
set(WAMR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/wasm-micro-runtime)
foreach(program ${PROGRAMS})
  add_executable(${program} ${program}.cpp)
endforeach(program)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${WAMR_DIR}/samples/socket-api/wasm-app-prefix/src/wasm-app-build/libsocket_wasi_ext.a -I${WAMR_DIR}/core/iwasm/libraries/lib-socket/inc/")