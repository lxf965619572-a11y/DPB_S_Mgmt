#include "common.h"
#include "config.h"
#include "logger.h"
#include "tcp_client.h"
#include "uart_client.h"
#include "cpri_protocol.h"
#include "fpga_protocol.h"
#include "msg_handler.h"
#include "fpga_handler.h"
#include "fpga_to_cpri.h"
#include "channel_setup.h"
#include "heartbeat.h"
#include "cell_config.h"
#include "alarm_manager.h"
#include "param_query.h"
#include "param_config.h"
#include "log_upload.h"
#include "version_manager.h"
#include "alarm_query.h"
#include "loopback.h"
#include "reset.h"
#include "transparent_msg.h"
#include "phased_array_calib.h"
#include "rs422_protocol.h"
#include "firmware_package.h"
#include "uart_rs422_client.h"
#include "fpga_firmware_injector.h"
#include "fpga_watchdog.h"
#include "telemetry_log.h"
#include <signal.h>

tcp_client_t g_tcp_client;  /* 全局TCP客户端，供msg_handler使用 */
uart_client_t g_uart_client;  /* 全局UART客户端，供fpga_handler使用 */
uart_rs422_client_t g_uart_rs422_client;  /* RS-422 UART客户端(用于FPGA固件上注) */
/* 由信号处理器写，必须是 volatile sig_atomic_t 才保证读写原子、不被优化掉 */
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_exit_signal = 0;   /* 触发退出的信号号，主循环外再打日志 */
static volatile bool g_channel_established = false;  /* 通道建立标志 */
uint32_t g_serial_num = 0;  /* PAAU侧流水号 */
pthread_mutex_t g_serial_num_mutex = PTHREAD_MUTEX_INITIALIZER;  /* 保护流水号（全局） */
static uint8_t g_recv_buffer[8192];
static uint32_t g_recv_offset = 0;

/* 获取下一个流水号（线程安全） */
static inline uint32_t get_next_serial_num(void)
{
    pthread_mutex_lock(&g_serial_num_mutex);
    uint32_t serial = ++g_serial_num;
    pthread_mutex_unlock(&g_serial_num_mutex);
    return serial;
}

/* 信号处理 */
static void signal_handler(int signo)
{
    if (signo == SIGINT || signo == SIGTERM) {
        /* 信号处理器里只做 async-signal-safe 的事：置标志。
         * 原先这里还调用 LOG_INFO —— 它会取 g_log_mutex 并做 stdio 文件 I/O，
         * 若信号恰好打断了一个正持有 g_log_mutex 的线程（主线程自己也在打日志），
         * 就会在同一线程上二次加锁而永久自死锁，进程既不退出也不再工作。
         * 日志移到主循环结束后打印。 */
        g_exit_signal = signo;
        g_running = 0;
    }
}

/* TCP连接成功回调 */
static void on_tcp_connected(void)
{
    LOG_INFO("TCP connected to BBU");

    /* 重置通道建立标志 */
    g_channel_established = false;

    /* 立即发送第一次通道建立请求 */
    cpri_message_t request;
    int ret = channel_setup_create_request(&request, CHANNEL_SETUP_REASON_POWER_ON);
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to create channel setup request");
        return;
    }

    /* 设置流水号 */
    request.header.serial_num = get_next_serial_num();

    /* 编码并发送请求 */
    uint8_t buffer[2048];
    int len = cpri_encode_message(&request, buffer, sizeof(buffer));
    if (len > 0) {
        tcp_client_send(&g_tcp_client, buffer, len);
        LOG_INFO("Sent channel setup request (serial_num=%u, %d bytes)",
                 request.header.serial_num, len);
    }

    cpri_free_message(&request);
}

/* TCP断开连接回调 */
static void on_tcp_disconnected(void)
{
    LOG_WARN("TCP disconnected from BBU");
    /* 丢弃上一条连接残留的半帧。
     * 断链时缓冲里可能还有不完整的报文（1~8191 字节），若不清掉，重连后新连接
     * 的首批字节会被拼接到这段残留后面，按错位的长度字段去解析，轻则丢掉
     * 新连接的首条报文（通道建立配置），重则把拼出来的垃圾当成合法报文分发。
     * 线程安全：本回调在接收线程的退出段执行，与 on_tcp_data_received 是同一
     * 线程；下一个接收线程要等本线程被 join 之后才会创建（见 tcp_connect_to_server），
     * 因此这里无需加锁。 */
    g_recv_offset = 0;
    /* 重置通道建立标志 */
    g_channel_established = false;
    /* 停止心跳 */
    heartbeat_stop();
    /* 与基带无法通信：给天线发关闭发射指令 */
    cell_config_notify_bbu_link(false, "TCP disconnected from BBU");
}

/* TCP数据接收回调 */
static void on_tcp_data_received(const uint8_t *data, uint32_t len)
{
    /* 将接收到的数据追加到缓冲区 */
    if (g_recv_offset + len > sizeof(g_recv_buffer)) {
        LOG_ERROR("Receive buffer overflow (offset=%u, len=%u, buffer_size=%zu), resetting",
                  g_recv_offset, len, sizeof(g_recv_buffer));
        g_recv_offset = 0;
        return;
    }

    memcpy(g_recv_buffer + g_recv_offset, data, len);
    g_recv_offset += len;

    // LOG_DEBUG("TCP received %u bytes, buffer now has %u bytes", len, g_recv_offset);

    /* 循环处理缓冲区中的所有完整消息（粘包处理） */
    while (g_recv_offset >= CPRI_HEADER_LEN) {
        /* 读取消息长度字段（D2000 CPU是小端序，直接读取） */
        uint32_t msg_length;
        memcpy(&msg_length, g_recv_buffer + 4, 4);

        /* 验证消息长度的合理性，防止异常数据 */
        if (msg_length < CPRI_HEADER_LEN || msg_length > sizeof(g_recv_buffer)) {
            LOG_ERROR("Invalid CPRI message length: %u (expected %d ~ %zu), resetting buffer",
                      msg_length, CPRI_HEADER_LEN, sizeof(g_recv_buffer));
            g_recv_offset = 0;
            break;
        }

        /* 检查是否接收到完整消息（半包处理） */
        if (g_recv_offset < msg_length) {
            LOG_DEBUG("Incomplete message: have %u bytes, need %u bytes, waiting for more data",
                      g_recv_offset, msg_length);
            break;  /* 等待更多数据 */
        }

        /* 解码完整消息 */
        cpri_message_t msg;
        int ret = cpri_decode_message(g_recv_buffer, msg_length, &msg);
        if (ret == SUCCESS) {
            // LOG_DEBUG("Decoded CPRI message: msg_id=%u, length=%u, serial_num=%u",
            //           msg.header.msg_id, msg.header.msg_length, msg.header.serial_num);

            /* 检查是否为通道建立配置消息 */
            if (msg.header.msg_id == MSG_CHANNEL_SETUP_CFG) {
                LOG_INFO("Received channel setup config, stopping periodic requests");
                g_channel_established = true;
                /* 基带通道重新建立，放开"允许开发射"的闸门。
                 * 此处不直接开射：实际开射仍等基带重新下发频点配置。 */
                cell_config_notify_bbu_link(true, "channel established with BBU");
                /* 与 BBU 通道建立 = 当前软件版本可用：结束升级试运行（若有） */
                version_trial_on_channel_established();
            }

            /* 分发消息处理 */
            msg_handler_dispatch(&msg);
            cpri_free_message(&msg);
        } else {
            LOG_ERROR("Failed to decode CPRI message (ret=%d), skipping this message", ret);
        }

        /* 移除已处理的消息 */
        g_recv_offset -= msg_length;
        if (g_recv_offset > 0) {
            memmove(g_recv_buffer, g_recv_buffer + msg_length, g_recv_offset);
            // LOG_DEBUG("Moved %u bytes to buffer start, continue processing", g_recv_offset);
        }
    }
}

/* UART数据接收回调 */
static void on_uart_data_received(const uint8_t *data, uint32_t len)
{
    (void)data;  /* 未使用,由uart_client内部处理 */
    LOG_DEBUG("UART data received: %u bytes", len);
    /* 由uart_client内部处理帧解析 */
}

/* FPGA状态接收回调 */
static void on_fpga_status_received(const void *status)
{
    if (!status) {
        LOG_ERROR("Received NULL FPGA status pointer");
        return;
    }

    const fpga_status_frame_t *fpga_status = (const fpga_status_frame_t*)status;

    /* 更新FPGA状态管理器 */
    fpga_handler_on_status_update(fpga_status);
}

int main(int argc, char *argv[])
{
    int ret;
    const char *config_file = "./config/antenna_mgmt.conf";

    printf("========================================\n");
    printf("  S频段天线管理面软件 v1.0\n");
    printf("========================================\n\n");

    /* 加载配置文件 */
    if (argc > 1) {
        config_file = argv[1];
    }

    ret = config_load(config_file);
    if (ret != SUCCESS) {
        fprintf(stderr, "Failed to load config file: %s\n", config_file);
        return -1;
    }

    /* 初始化日志 */
    const char *log_file = config_get_string("LOG_FILE", "./logs/antenna_mgmt.log");
    ret = logger_init(log_file, g_config.log_level);
    if (ret != SUCCESS) {
        fprintf(stderr, "Failed to init logger\n");
        return -1;
    }

    LOG_INFO("=== S-Band Antenna Management Software Started ===");
    LOG_INFO("BBU Server: %s:%d", g_config.bbu_ip, g_config.bbu_port);
    LOG_INFO("PAAU Client: %s", g_config.paau_ip);

    /* 初始化FPGA处理器 */
    ret = fpga_handler_init();
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to init FPGA handler");
        goto cleanup;
    }

    /* 初始化消息处理器 */
    ret = msg_handler_init();
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to init message handler");
        goto cleanup;
    }

    /* 初始化心跳管理器 */
    ret = heartbeat_init();
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to init heartbeat manager");
        goto cleanup;
    }

    /* 初始化小区配置管理器 */
    ret = cell_config_init();
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to init cell config manager");
        goto cleanup;
    }

    /* 初始化告警管理器 */
    alarm_config_t alarm_config = {
        .channel_fault_threshold = 10,
        .over_temp_threshold_high = 85,
        .over_temp_threshold_low = -40,
        /* 光链路告警启动宽限期：上电后 CPRI 光链路还在协商，这段时间
         * link_success_flag 本来就读 0，不设宽限会把正常建链过程报成
         * 4 条 1097 告警。从第一帧 FPGA 遥测起算，0=关闭。 */
        .link_grace_sec = config_get_uint32("ALARM_LINK_GRACE_SEC", 10),
        /* 1097 监测光口数：4=仅主光口（默认），8=主+备光口。按硬件装配情况配置 */
        .optical_port_count = (uint8_t)config_get_uint32("ALARM_OPTICAL_PORTS", 4)
    };
    ret = alarm_manager_init(&alarm_config);
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to init alarm manager");
        goto cleanup;
    }

    /* 初始化参数查询模块 */
    ret = param_query_init();
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to init param query module");
        goto cleanup;
    }

    /* 初始化参数配置模块 */
    ret = param_config_init();
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to init param config module");
        goto cleanup;
    }

    /* 初始化日志上传模块 */
    ret = log_upload_init();
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to init log upload module");
        goto cleanup;
    }

    /* 初始化版本管理模块 */
    ret = version_manager_init();
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to init version manager");
        goto cleanup;
    }

    /* 初始化告警查询模块 */
    ret = alarm_query_init();
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to init alarm query module");
        goto cleanup;
    }

    /* 初始化环回模块 */
    ret = loopback_init();
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to init loopback module");
        goto cleanup;
    }

    /* 初始化复位模块 */
    ret = reset_init();
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to init reset module");
        goto cleanup;
    }

    /* 初始化透传消息模块 */
    ret = transparent_msg_init();
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to init transparent message module");
        goto cleanup;
    }
    
    /* 初始化相控阵校准模块 */
    ret = phased_array_calib_init();
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to init phased array calibration module");
        goto cleanup;
    }

    /* 初始化RS-422协议层 */
    ret = rs422_protocol_init();
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to init RS-422 protocol");
        goto cleanup;
    }

    /* 初始化固件包解析模块 */
    ret = firmware_package_init();
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to init firmware package module");
        goto cleanup;
    }

    /* 初始化RS-422 UART客户端 */
    ret = uart_rs422_init(&g_uart_rs422_client, "/dev/ttyAMA2");
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to init RS-422 UART client");
        goto cleanup;
    }

    ret = uart_rs422_open(&g_uart_rs422_client);
    if (ret != SUCCESS) {
        LOG_WARN("Failed to open RS-422 UART (FPGA firmware injection will be unavailable)");
        /* 非致命错误,继续运行 */
    } else {
        /* 初始化FPGA固件注入器 */
        ret = fpga_firmware_injection_init(&g_uart_rs422_client);
        if (ret != SUCCESS) {
            LOG_ERROR("Failed to init FPGA firmware injector");
            /* 非致命错误,继续运行 */
        } else {
            LOG_INFO("FPGA firmware injection system initialized");
        }
    }

    /* 初始化UART客户端 */
    const char *uart_device = config_get_string("UART_DEVICE", UART_DEVICE);
    ret = uart_client_init(&g_uart_client, uart_device);
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to init UART client");
        goto cleanup;
    }

    /* 设置UART回调函数 */
    g_uart_client.on_data_received = on_uart_data_received;
    g_uart_client.on_status_received = on_fpga_status_received;
    /* 必须在下方的 uart_client_open 之前装配好：接收线程一旦起来，早到的
     * 0xEB93 遥测日志帧就会因为回调为空而被丢弃 */
    g_uart_client.on_telemetry_log_frame = telemetry_log_on_frame;

    /* 打开UART设备 */
    ret = uart_client_open(&g_uart_client);
    if (ret != SUCCESS) {
        LOG_WARN("Failed to open UART device (will continue without FPGA communication)");
        /* 不退出，继续运行TCP部分 */
    }

    /* 初始化遥测日志（每分钟轮询 V7A/V7B1/V7B2 并把回复的 0xEB93 帧落盘）。
     * 放在 uart_client_open 之后：轮询线程要经 UART 下发命令。
     * 放在 tcp_client_start 之前：基带随时可能下发日志上传请求，而上传打包时要读
     * 遥测日志文件，先把依赖就绪。
     * 失败不 goto cleanup：遥测日志是旁路功能，不该拖垮主服务（与上面 UART 打不开
     * 的处理一致）。 */
    ret = telemetry_log_init();
    if (ret != SUCCESS) {
        LOG_WARN("Failed to init telemetry log (will continue without telemetry logging)");
    }

    /* 初始化FPGA看门狗 */
    uint32_t watchdog_gpio_base = config_get_uint32("WATCHDOG_GPIO_BASE", GPIO0_BASE);
    uint32_t watchdog_pin_mux_offset = config_get_uint32("WATCHDOG_PIN_MUX_OFFSET", GPIO0_B6_MUX_OFFSET);
    uint32_t watchdog_mux_func_bit = config_get_uint32("WATCHDOG_MUX_FUNC_BIT", GPIO0_B6_FUNC_BIT);
    uint32_t watchdog_mux_pull_bit = config_get_uint32("WATCHDOG_MUX_PULL_BIT", GPIO0_B6_PULL_BIT);
    uint32_t watchdog_port = config_get_uint32("WATCHDOG_GPIO_PORT", 6);  /* GPIO0_B6 */
    uint32_t watchdog_use_portb = config_get_uint32("WATCHDOG_USE_PORTB", 1);  /* 使用PORTB组 */

    ret = fpga_watchdog_init(watchdog_gpio_base, watchdog_pin_mux_offset,
                             watchdog_mux_func_bit, watchdog_mux_pull_bit,
                             watchdog_port, watchdog_use_portb);
    if (ret != SUCCESS) {
        LOG_WARN("Failed to init FPGA watchdog (will continue without watchdog)");
        /* 非致命错误，继续运行 */
    } else {
        /* 启动看门狗喂狗线程 */
        ret = fpga_watchdog_start();
        if (ret != SUCCESS) {
            LOG_WARN("Failed to start FPGA watchdog thread");
        } else {
            LOG_INFO("FPGA watchdog started successfully");
        }
    }

    /* 初始化TCP客户端 */
    ret = tcp_client_init(&g_tcp_client, g_config.bbu_ip, g_config.bbu_port);
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to init TCP client");
        goto cleanup;
    }

    /* 设置回调函数 */
    g_tcp_client.on_connected = on_tcp_connected;
    g_tcp_client.on_disconnected = on_tcp_disconnected;
    g_tcp_client.on_data_received = on_tcp_data_received;

    /* 启动TCP客户端 */
    ret = tcp_client_start(&g_tcp_client);
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to start TCP client");
        goto cleanup;
    }

    /* 注册信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 忽略 SIGPIPE：tcp_client_send 的 send() 未带 MSG_NOSIGNAL，
     * 对端 RST 后写入会触发 SIGPIPE，默认处置是终止进程，
     * 期间 FPGA 看门狗喂狗线程停止（可能触发硬件复位）。 */
    signal(SIGPIPE, SIG_IGN);

    LOG_INFO("System running, press Ctrl+C to exit");

    /* 主循环 */
    int channel_setup_timer = 0;
    int heartbeat_timer = 0;
    uint32_t uptime_sec = 0;
    while (g_running) {
        sleep(1);
        channel_setup_timer++;
        heartbeat_timer++;

        /* 升级试运行：稳定运行达到 UPGRADE_TRIAL_STABLE_SEC 后清零启动计数 */
        version_trial_on_uptime(++uptime_sec);

        /* 如果通道未建立且TCP已连接，每5秒发送一次通道建立请求 */
        if (!g_channel_established && tcp_client_get_state(&g_tcp_client) == TCP_STATE_CONNECTED) {
            if (channel_setup_timer >= 5) {
                channel_setup_timer = 0;

                cpri_message_t request;
                int ret = channel_setup_create_request(&request, CHANNEL_SETUP_REASON_RECONNECT);
                if (ret == SUCCESS) {
                    request.header.serial_num = get_next_serial_num();

                    uint8_t buffer[2048];
                    int len = cpri_encode_message(&request, buffer, sizeof(buffer));
                    if (len > 0) {
                        tcp_client_send(&g_tcp_client, buffer, len);
                        LOG_INFO("Periodic channel setup request sent (serial_num=%u)",
                                 request.header.serial_num);
                    }
                    cpri_free_message(&request);
                }
            }
        } else {
            /* 通道已建立，重置定时器 */
            channel_setup_timer = 0;
        }

        /* 如果通道已建立，每3秒发送一次心跳 */
        if (g_channel_established && tcp_client_get_state(&g_tcp_client) == TCP_STATE_CONNECTED) {
            if (heartbeat_timer >= HEARTBEAT_INTERVAL_SEC) {
                heartbeat_timer = 0;
                heartbeat_send_paau();
            }

            /* 检查BBU心跳超时 */
            if (heartbeat_check_timeout()) {
                LOG_ERROR("BBU heartbeat timeout, resetting connection");
                /* 生成告警并重连 */
                g_channel_established = false;
                heartbeat_stop();
                /* TCP还连着但基带不再应答，同样属于"与基带无法通信"：
                 * 给天线发关闭发射指令。恢复要等基带重新完成通道建立。 */
                cell_config_notify_bbu_link(false, "BBU heartbeat timeout");
                /* TCP会自动重连，重新进入通道建立流程 */
            }
        } else {
            /* 通道未建立，重置心跳定时器 */
            heartbeat_timer = 0;
        }
    }

cleanup:
    if (g_exit_signal != 0) {
        LOG_INFO("Received signal %d, shutting down...", (int)g_exit_signal);
    }
    LOG_INFO("Shutting down...");

    /* 停止FPGA看门狗 */
    fpga_watchdog_stop();
    fpga_watchdog_destroy();

    /* 清理FPGA固件上注模块。
     * 注入线程若未能在限时内退出（卡在 RS-422 收发里），它仍在使用串口 fd 与
     * RS-422 协议层资源；此时再 close 串口 / cleanup 协议层就是 use-after-free。
     * 进程马上就要退出，这些资源交给内核回收即可。 */
    if (fpga_firmware_injection_cleanup() == SUCCESS) {
        uart_rs422_close(&g_uart_rs422_client);
        firmware_package_cleanup();
        rs422_protocol_cleanup();
    } else {
        LOG_WARN("FPGA injection thread still alive, leaving RS-422 resources to process exit");
    }

    heartbeat_stop();

    /* cell_config 的 worker 线程会在响应时调用 tcp_client_send 并读 fpga_handler，
     * 所以必须在拆 TCP 客户端与 fpga_handler 之前先停它（内部 join，可能等一次 5 秒模式校验）。
     * 否则 worker 可能在 send_mutex 被销毁后仍去锁它。 */
    cell_config_destroy();

    /* telemetry_log 的轮询线程会调 uart_client_send，所以必须在 uart_client_close
     * 之前停掉并 join（否则 uart_client_destroy 销毁 send_mutex 后线程仍可能去锁它）。
     * 放在这里而不是更早：与 cell_config 一样属于"有后台线程的模块"统一排在这一段的约定。
     * 此后 UART 接收线程可能仍在跑并回调 telemetry_log_on_frame，回调内部会检查 stop
     * 标志后静默返回，不会往已停用的队列里推数据。 */
    telemetry_log_destroy();

    /* log_upload 的上传线程会调 tcp_client_send，必须在 TCP 拆卸之前置停止位，
     * 否则线程可能撞上已销毁的 send_mutex（见 log_upload_destroy 的说明）。
     * 顺带放在 telemetry_log_destroy 之后：上传打包要读遥测日志文件。 */
    log_upload_destroy();

    /* version_manager 的下载线程同样会调 tcp_client_send（下载完成时发结果指示），
     * 所以也要在 TCP 拆卸之前置它的退出标志。原先本函数排在 tcp_client_destroy
     * 之后，标志加在 destroy 里也来不及。它不 join（detached + curl 可能跑 600 秒），
     * 只缩小竞态窗口，与 log_upload 同一取舍。 */
    version_manager_destroy();

    tcp_client_stop(&g_tcp_client);
    tcp_client_destroy(&g_tcp_client);
    uart_client_close(&g_uart_client);
    uart_client_destroy(&g_uart_client);
    fpga_handler_destroy();
    heartbeat_destroy();
    alarm_manager_destroy();
    alarm_query_cleanup();
    loopback_cleanup();
    reset_cleanup();
    transparent_msg_cleanup();
    phased_array_calib_cleanup();
    msg_handler_destroy();
    logger_close();

    LOG_INFO("=== S-Band Antenna Management Software Stopped ===");
    return 0;
}
