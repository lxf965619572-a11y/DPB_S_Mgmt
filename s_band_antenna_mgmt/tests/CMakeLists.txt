# 查找Google Test (包含GMock)
find_package(GTest REQUIRED)
find_package(OpenSSL REQUIRED)
include_directories(${GTEST_INCLUDE_DIRS})

# 测试通用设置
set(TEST_LINK_LIBS
    antenna_mgmt_lib
    ${GTEST_BOTH_LIBRARIES}
    gmock
    gmock_main
    pthread
    ${OPENSSL_CRYPTO_LIBRARY}
)

# 单元测试宏
macro(add_unit_test test_name test_source)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} ${TEST_LINK_LIBS})
    target_include_directories(${test_name} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src
    )
    add_test(NAME ${test_name} COMMAND ${test_name}
        --gtest_output=xml:${CMAKE_BINARY_DIR}/../reports/xml/${test_name}.xml)
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 60)
endmacro()

# 添加单元测试
add_unit_test(test_rs422_protocol unit/test_rs422_protocol.cpp)
add_unit_test(test_fpga_firmware_injector unit/test_fpga_firmware_injector.cpp)
add_unit_test(test_fpga_firmware_injector_integration unit/test_fpga_firmware_injector_integration.cpp)
add_unit_test(test_fpga_injector_advanced unit/test_fpga_injector_advanced.cpp)
add_unit_test(test_firmware_package unit/test_firmware_package.cpp)

# 自定义测试目标
add_custom_target(run_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure --verbose
    DEPENDS test_rs422_protocol test_fpga_firmware_injector
            test_fpga_firmware_injector_integration test_fpga_injector_advanced
            test_firmware_package
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# 覆盖率目标
if(CMAKE_BUILD_TYPE STREQUAL "Coverage")
    add_custom_target(coverage
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/../reports/html
        COMMAND lcov --directory ${CMAKE_BINARY_DIR} --zerocounters --rc lcov_branch_coverage=1
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        COMMAND lcov --directory ${CMAKE_BINARY_DIR} --capture --output-file coverage.info --rc lcov_branch_coverage=1
        COMMAND lcov --remove coverage.info '/usr/*' '*/tests/*' --output-file coverage_filtered.info --rc lcov_branch_coverage=1
        COMMAND genhtml coverage_filtered.info --output-directory ${CMAKE_BINARY_DIR}/../reports/html --branch-coverage --rc genhtml_branch_coverage=1
        COMMAND lcov --summary coverage_filtered.info --rc lcov_branch_coverage=1
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Generating coverage report..."
    )
endif()
