cmake_minimum_required(VERSION 4.2)
project(CCloth)

set(CMAKE_CXX_STANDARD 20)

file(GLOB_RECURSE CCloth_CPP_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/*.cpp"
)

file(GLOB_RECURSE CCloth_C_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/*.c"
)

add_executable(CCloth
        ${CCloth_CPP_SOURCES}
        ${CCloth_C_SOURCES}
)

target_include_directories(CCloth PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
)
