cmake_minimum_required(VERSION 3.10)
project(s_band_antenna_mgmt C CXX)

# 设置C/C++标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置编译选项
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")

# Debug模式
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g -O0")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")

# Coverage模式 - 定义为自定义构建类型
set(CMAKE_C_FLAGS_COVERAGE "-g -O0 --coverage -fprofile-arcs -ftest-coverage" CACHE STRING "Flags used by the C compiler during coverage builds." FORCE)
set(CMAKE_CXX_FLAGS_COVERAGE "-g -O0 --coverage -fprofile-arcs -ftest-coverage" CACHE STRING "Flags used by the C++ compiler during coverage builds." FORCE)
set(CMAKE_EXE_LINKER_FLAGS_COVERAGE "--coverage -lgcov" CACHE STRING "Flags used for linking binaries during coverage builds." FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_COVERAGE "--coverage -lgcov" CACHE STRING "Flags used by the shared libraries linker during coverage builds." FORCE)
mark_as_advanced(
    CMAKE_CXX_FLAGS_COVERAGE
    CMAKE_C_FLAGS_COVERAGE
    CMAKE_EXE_LINKER_FLAGS_COVERAGE
    CMAKE_SHARED_LINKER_FLAGS_COVERAGE
)

# 将Coverage添加到可用的构建类型列表
set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Coverage" CACHE STRING "Available build types" FORCE)

# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 包含目录
include_directories(
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)

# 源文件列表
set(SOURCES
    src/rs422_protocol.c
    src/logger.c
    src/fpga_firmware_injector.c
    src/firmware_package.c
)

# 查找OpenSSL
find_package(OpenSSL REQUIRED)

# 创建静态库(用于测试)
add_library(antenna_mgmt_lib STATIC ${SOURCES})
target_link_libraries(antenna_mgmt_lib pthread ${OPENSSL_CRYPTO_LIBRARY})

# 启用测试
enable_testing()

# 添加测试子目录
add_subdirectory(tests)

# 添加便捷目标
if(CMAKE_BUILD_TYPE STREQUAL "Coverage")
    # 一键运行测试和生成覆盖率报告
    add_custom_target(all-with-coverage
        COMMAND ${CMAKE_COMMAND} --build . --target all
        COMMAND ${CMAKE_COMMAND} --build . --target coverage
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Building, testing, and generating coverage report..."
    )
endif()

