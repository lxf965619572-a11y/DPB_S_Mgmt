/**
 * @file test_fpga_firmware_injector.cpp
 * @brief FPGA固件上注器详细单元测试
 *
 * 测试覆盖率目标:
 * - 语句覆盖率 ≥ 90%
 * - 分支覆盖率 ≥ 85%
 * - MC/DC覆盖率 ≥ 60%
 *
 * 每个函数设计 ≥3 组典型用例（含正常路径、边界值、非法输入）
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <cstring>
#include <thread>
#include <chrono>

extern "C" {
#include "fpga_firmware_injector.h"
#include "uart_rs422_client.h"
#include "firmware_package.h"
#include "common.h"
}

using ::testing::_;
using ::testing::Return;
using ::testing::DoAll;
using ::testing::SetArgPointee;

// ============ Mock UART客户端 ============

class MockUARTClient {
public:
    MOCK_METHOD4(send_and_wait, int(uart_rs422_client_t*, const uint8_t*, uint32_t, uint16_t));
    MOCK_METHOD3(send_frame, int(uart_rs422_client_t*, const uint8_t*, uint32_t));
};

static MockUARTClient* g_mock_uart = nullptr;

// Mock函数实现
extern "C" {
    int uart_rs422_send_and_wait(uart_rs422_client_t *client,
                                  const uint8_t *send_buf, uint32_t send_len,
                                  uint8_t *recv_buf, uint32_t recv_buf_size,
                                  uint32_t *recv_len, uint16_t expected_cmd) {
        if (g_mock_uart) {
            return g_mock_uart->send_and_wait(client, send_buf, send_len, expected_cmd);
        }
        return ERROR_GENERAL;
    }

    int uart_rs422_send_frame(uart_rs422_client_t *client,
                              const uint8_t *frame_buf, uint32_t frame_len) {
        if (g_mock_uart) {
            return g_mock_uart->send_frame(client, frame_buf, frame_len);
        }
        return ERROR_GENERAL;
    }
}

// ============ 测试夹具 ============

class FPGAFirmwareInjectorTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_mock_uart = new MockUARTClient();

        // 创建模拟UART客户端
        uart_client = (uart_rs422_client_t*)malloc(sizeof(uart_rs422_client_t));
        memset(uart_client, 0, sizeof(uart_rs422_client_t));

        // 初始化注入器
        ASSERT_EQ(SUCCESS, fpga_firmware_injection_init(uart_client));
    }

    void TearDown() override {
        fpga_firmware_injection_cleanup();

        if (uart_client) {
            free(uart_client);
            uart_client = nullptr;
        }

        delete g_mock_uart;
        g_mock_uart = nullptr;
    }

    uart_rs422_client_t* uart_client;
};

// ============ 初始化和清理测试 ============

TEST_F(FPGAFirmwareInjectorTest, InitSuccess) {
    // 在SetUp中已经初始化,验证状态
    uint32_t current, total;
    fpga_injection_state_t state;

    int ret = fpga_firmware_injection_get_progress(&current, &total, &state);
    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(FPGA_INJ_STATE_IDLE, state);
}

TEST_F(FPGAFirmwareInjectorTest, InitNullClient) {
    // 测试NULL客户端初始化
    fpga_firmware_injection_cleanup();

    int ret = fpga_firmware_injection_init(nullptr);
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(FPGAFirmwareInjectorTest, InitMultipleTimes) {
    // 测试多次初始化
    fpga_firmware_injection_cleanup();

    uart_rs422_client_t* client = (uart_rs422_client_t*)malloc(sizeof(uart_rs422_client_t));

    EXPECT_EQ(SUCCESS, fpga_firmware_injection_init(client));
    EXPECT_EQ(SUCCESS, fpga_firmware_injection_init(client));
    EXPECT_EQ(SUCCESS, fpga_firmware_injection_init(client));

    fpga_firmware_injection_cleanup();
    free(client);
}

// ============ 状态名称测试 ============

TEST_F(FPGAFirmwareInjectorTest, GetStateNameAllStates) {
    // 测试所有状态名称
    EXPECT_STREQ("空闲", fpga_injection_get_state_name(FPGA_INJ_STATE_IDLE));
    EXPECT_STREQ("解析元数据", fpga_injection_get_state_name(FPGA_INJ_STATE_PARSING_METADATA));
    EXPECT_STREQ("传输开始", fpga_injection_get_state_name(FPGA_INJ_STATE_TRANSFER_START));
    EXPECT_STREQ("传输数据", fpga_injection_get_state_name(FPGA_INJ_STATE_TRANSFER_DATA));
    EXPECT_STREQ("传输结束", fpga_injection_get_state_name(FPGA_INJ_STATE_TRANSFER_END));
    EXPECT_STREQ("重构开始", fpga_injection_get_state_name(FPGA_INJ_STATE_RECONFIG_START));
    EXPECT_STREQ("重构轮询", fpga_injection_get_state_name(FPGA_INJ_STATE_RECONFIG_POLLING));
    EXPECT_STREQ("完成", fpga_injection_get_state_name(FPGA_INJ_STATE_COMPLETED));
    EXPECT_STREQ("失败", fpga_injection_get_state_name(FPGA_INJ_STATE_FAILED));
}

TEST_F(FPGAFirmwareInjectorTest, GetStateNameUnknown) {
    // 测试未知状态
    const char* name = fpga_injection_get_state_name((fpga_injection_state_t)999);
    EXPECT_STREQ("未知", name);
}

TEST_F(FPGAFirmwareInjectorTest, GetStateNameBoundary) {
    // 测试边界状态值
    const char* name1 = fpga_injection_get_state_name((fpga_injection_state_t)0);
    EXPECT_NE(nullptr, name1);

    const char* name2 = fpga_injection_get_state_name((fpga_injection_state_t)8);
    EXPECT_NE(nullptr, name2);
}

// ============ 进度查询测试 ============

TEST_F(FPGAFirmwareInjectorTest, GetProgressInitialState) {
    // 测试初始状态的进度
    uint32_t current, total;
    fpga_injection_state_t state;

    int ret = fpga_firmware_injection_get_progress(&current, &total, &state);

    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(0, current);
    EXPECT_EQ(0, total);
    EXPECT_EQ(FPGA_INJ_STATE_IDLE, state);
}

TEST_F(FPGAFirmwareInjectorTest, GetProgressNullParams) {
    // 测试NULL参数
    int ret = fpga_firmware_injection_get_progress(nullptr, nullptr, nullptr);
    EXPECT_EQ(SUCCESS, ret);  // 应该允许NULL参数
}

TEST_F(FPGAFirmwareInjectorTest, GetProgressPartialNull) {
    // 测试部分NULL参数
    uint32_t current;
    fpga_injection_state_t state;

    int ret = fpga_firmware_injection_get_progress(&current, nullptr, &state);
    EXPECT_EQ(SUCCESS, ret);
    EXPECT_EQ(FPGA_INJ_STATE_IDLE, state);
}

// ============ 启动注入测试 ============

TEST_F(FPGAFirmwareInjectorTest, StartInjectionNullVersionDir) {
    // 测试NULL版本目录
    int ret = fpga_firmware_injection_start(nullptr, "v1.0.0");
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(FPGAFirmwareInjectorTest, StartInjectionNullVersionNum) {
    // 测试NULL版本号
    int ret = fpga_firmware_injection_start("/path/to/firmware", nullptr);
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

TEST_F(FPGAFirmwareInjectorTest, StartInjectionBothNull) {
    // 测试两个参数都为NULL
    int ret = fpga_firmware_injection_start(nullptr, nullptr);
    EXPECT_EQ(ERROR_INVALID_PARAM, ret);
}

// ============ 中止注入测试 ============

TEST_F(FPGAFirmwareInjectorTest, AbortWhenNotInProgress) {
    // 测试未在进行时中止
    int ret = fpga_firmware_injection_abort();
    EXPECT_EQ(ERROR_GENERAL, ret);
}

TEST_F(FPGAFirmwareInjectorTest, AbortMultipleTimes) {
    // 测试多次中止
    int ret1 = fpga_firmware_injection_abort();
    EXPECT_EQ(ERROR_GENERAL, ret1);

    int ret2 = fpga_firmware_injection_abort();
    EXPECT_EQ(ERROR_GENERAL, ret2);
}

// ============ 等待完成测试 ============

TEST_F(FPGAFirmwareInjectorTest, WaitWhenNotInProgress) {
    // 测试未在进行时等待
    int ret = fpga_firmware_injection_wait(1);
    EXPECT_EQ(ERROR_GENERAL, ret);  // 应该立即返回失败
}

TEST_F(FPGAFirmwareInjectorTest, WaitWithZeroTimeout) {
    // 测试零超时(无限等待)
    // 注意: 这个测试会立即返回因为没有任务在进行
    int ret = fpga_firmware_injection_wait(0);
    EXPECT_EQ(ERROR_GENERAL, ret);
}

TEST_F(FPGAFirmwareInjectorTest, WaitWithShortTimeout) {
    // 测试短超时
    int ret = fpga_firmware_injection_wait(1);
    EXPECT_EQ(ERROR_GENERAL, ret);
}

// ============ 清理测试 ============

TEST_F(FPGAFirmwareInjectorTest, CleanupWhenIdle) {
    // 测试空闲状态清理
    fpga_firmware_injection_cleanup();
    // 不应该崩溃
    SUCCEED();
}

TEST_F(FPGAFirmwareInjectorTest, CleanupMultipleTimes) {
    // 测试多次清理
    fpga_firmware_injection_cleanup();
    fpga_firmware_injection_cleanup();
    fpga_firmware_injection_cleanup();
    // 不应该崩溃
    SUCCEED();
}

// ============ 状态转换测试 ============

TEST_F(FPGAFirmwareInjectorTest, StateTransitionSequence) {
    // 测试正常的状态转换序列
    // 这个测试验证状态名称在所有状态下都能正确返回

    fpga_injection_state_t states[] = {
        FPGA_INJ_STATE_IDLE,
        FPGA_INJ_STATE_PARSING_METADATA,
        FPGA_INJ_STATE_TRANSFER_START,
        FPGA_INJ_STATE_TRANSFER_DATA,
        FPGA_INJ_STATE_TRANSFER_END,
        FPGA_INJ_STATE_RECONFIG_START,
        FPGA_INJ_STATE_RECONFIG_POLLING,
        FPGA_INJ_STATE_COMPLETED,
        FPGA_INJ_STATE_FAILED
    };

    for (auto state : states) {
        const char* name = fpga_injection_get_state_name(state);
        EXPECT_NE(nullptr, name);
        EXPECT_GT(strlen(name), 0);
    }
}

// ============ 边界值测试 ============

TEST_F(FPGAFirmwareInjectorTest, LongVersionDir) {
    // 测试超长版本目录路径
    char long_path[300];
    memset(long_path, 'A', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';

    int ret = fpga_firmware_injection_start(long_path, "v1.0.0");
    // 启动成功,但后台线程会因为路径不存在而失败
    EXPECT_EQ(SUCCESS, ret);

    // 等待后台线程处理
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 检查状态应该变为FAILED
    fpga_injection_state_t state;
    fpga_firmware_injection_get_progress(nullptr, nullptr, &state);
    EXPECT_EQ(FPGA_INJ_STATE_FAILED, state);
}

TEST_F(FPGAFirmwareInjectorTest, LongVersionNum) {
    // 测试超长版本号
    char long_version[50];
    memset(long_version, 'V', sizeof(long_version) - 1);
    long_version[sizeof(long_version) - 1] = '\0';

    int ret = fpga_firmware_injection_start("/path/to/firmware", long_version);
    // 启动成功,但后台线程会因为路径不存在而失败
    EXPECT_EQ(SUCCESS, ret);

    // 等待后台线程处理
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 检查状态应该变为FAILED
    fpga_injection_state_t state;
    fpga_firmware_injection_get_progress(nullptr, nullptr, &state);
    EXPECT_EQ(FPGA_INJ_STATE_FAILED, state);
}

TEST_F(FPGAFirmwareInjectorTest, EmptyVersionDir) {
    // 测试空版本目录
    int ret = fpga_firmware_injection_start("", "v1.0.0");
    // 空字符串不是NULL,所以会通过参数检查
    EXPECT_EQ(SUCCESS, ret);

    // 等待后台线程处理
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 检查状态应该变为FAILED
    fpga_injection_state_t state;
    fpga_firmware_injection_get_progress(nullptr, nullptr, &state);
    EXPECT_EQ(FPGA_INJ_STATE_FAILED, state);
}

TEST_F(FPGAFirmwareInjectorTest, EmptyVersionNum) {
    // 测试空版本号
    int ret = fpga_firmware_injection_start("/path/to/firmware", "");
    // 空字符串不是NULL,所以会通过参数检查
    EXPECT_EQ(SUCCESS, ret);

    // 等待后台线程处理
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 检查状态应该变为FAILED
    fpga_injection_state_t state;
    fpga_firmware_injection_get_progress(nullptr, nullptr, &state);
    EXPECT_EQ(FPGA_INJ_STATE_FAILED, state);
}

// ============ 并发测试 ============

TEST_F(FPGAFirmwareInjectorTest, ConcurrentProgressQuery) {
    // 测试并发进度查询
    std::vector<std::thread> threads;

    for (int i = 0; i < 10; i++) {
        threads.emplace_back([this]() {
            uint32_t current, total;
            fpga_injection_state_t state;

            for (int j = 0; j < 100; j++) {
                fpga_firmware_injection_get_progress(&current, &total, &state);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    SUCCEED();
}

TEST_F(FPGAFirmwareInjectorTest, ConcurrentAbort) {
    // 测试并发中止
    std::vector<std::thread> threads;

    for (int i = 0; i < 5; i++) {
        threads.emplace_back([this]() {
            fpga_firmware_injection_abort();
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    SUCCEED();
}

// ============ 超时测试 ============

TEST_F(FPGAFirmwareInjectorTest, WaitTimeoutBehavior) {
    // 测试等待超时行为
    auto start = std::chrono::steady_clock::now();

    int ret = fpga_firmware_injection_wait(2);  // 2秒超时

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(end - start);

    EXPECT_EQ(ERROR_GENERAL, ret);  // 没有任务在进行
    EXPECT_LT(duration.count(), 2);  // 应该立即返回
}

// ============ 错误处理测试 ============

TEST_F(FPGAFirmwareInjectorTest, InvalidStateTransition) {
    // 测试无效的状态转换
    // 通过获取状态名称来验证所有状态都有效
    for (int i = -1; i <= 10; i++) {
        const char* name = fpga_injection_get_state_name((fpga_injection_state_t)i);
        EXPECT_NE(nullptr, name);
    }
}

// ============ 资源管理测试 ============

TEST_F(FPGAFirmwareInjectorTest, CleanupWithoutInit) {
    // 测试未初始化就清理
    fpga_firmware_injection_cleanup();
    fpga_firmware_injection_cleanup();  // 第二次清理

    // 不应该崩溃
    SUCCEED();
}

TEST_F(FPGAFirmwareInjectorTest, InitAfterCleanup) {
    // 测试清理后重新初始化
    fpga_firmware_injection_cleanup();

    uart_rs422_client_t* client = (uart_rs422_client_t*)malloc(sizeof(uart_rs422_client_t));
    int ret = fpga_firmware_injection_init(client);

    EXPECT_EQ(SUCCESS, ret);

    fpga_firmware_injection_cleanup();
    free(client);
}

// ============ 主函数 ============

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
