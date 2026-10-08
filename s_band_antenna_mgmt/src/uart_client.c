#include <termios.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "uart_client.h"
#include "fpga_protocol.h"
#include "logger.h"

static void* uart_recv_thread(void *arg);
static void* uart_poll_thread(void *arg);

/* 遥测日志帧(0xEB93)被拒时的日志限频。
 * 乱码持续涌进来时，每个"看起来像帧头"的候选都打一条 WARN，会把只有 10MB 就轮转的
 * 文本日志刷满、把真正的故障记录挤出去，而且每条 WARN 都要抢 g_log_mutex、拖慢接收线程。
 * 策略：前 10 条全报（便于定位首次异常），之后每 100 条报 1 条。
 * 只由 UART 接收线程访问，故不需要加锁。 */
static uint32_t g_tel_reject_count = 0;

static bool telemetry_reject_should_log(void)
{
    g_tel_reject_count++;
    return (g_tel_reject_count <= 10) || (g_tel_reject_count % 100 == 0);
}

int uart_client_init(uart_client_t *client, const char *device)
{
    if (!client || !device) {
        return ERROR_INVALID_PARAM;
    }

    memset(client, 0, sizeof(uart_client_t));
    strncpy(client->device, device, sizeof(client->device) - 1);
    client->fd = -1;
    client->state = UART_STATE_CLOSED;
    client->running = false;

    if (pthread_mutex_init(&client->send_mutex, NULL) != 0) {
        LOG_ERROR("Failed to init UART mutex");
        return ERROR_GENERAL;
    }

    LOG_INFO("UART client initialized: %s", device);
    return SUCCESS;
}

int uart_client_open(uart_client_t *client)
{
    if (!client) {
        return ERROR_INVALID_PARAM;
    }

    /* 打开串口设备 */
    client->fd = open(client->device, O_RDWR | O_NOCTTY | O_NDELAY);
    if (client->fd < 0) {
        LOG_ERROR("Failed to open UART device %s: %s", client->device, strerror(errno));
        return ERROR_GENERAL;
    }

    /* 配置串口参数 */
    struct termios options;
    if (tcgetattr(client->fd, &options) != 0) {
        LOG_ERROR("Failed to get UART attributes: %s", strerror(errno));
        close(client->fd);
        client->fd = -1;
        return ERROR_GENERAL;
    }

    /* 设置波特率 */
    cfsetispeed(&options, UART_BAUDRATE);
    cfsetospeed(&options, UART_BAUDRATE);

    /* 设置数据位、停止位、校验位 */
    options.c_cflag &= ~CSIZE;
    options.c_cflag |= CS8;         /* 8位数据位 */
    options.c_cflag &= ~CSTOPB;     /* 1位停止位 */
    options.c_cflag &= ~PARENB;     /* 无校验 */
    options.c_cflag |= CLOCAL | CREAD;

    /* 设置为原始模式 */
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    options.c_oflag &= ~OPOST;
    options.c_iflag &= ~(IXON | IXOFF | IXANY);
    options.c_iflag &= ~(INLCR | ICRNL | IGNCR);

    /* 设置超时 */
    options.c_cc[VTIME] = 1;  /* 0.1秒超时 */
    options.c_cc[VMIN] = 0;   /* 非阻塞读取 */

    /* 应用配置 */
    if (tcsetattr(client->fd, TCSANOW, &options) != 0) {
        LOG_ERROR("Failed to set UART attributes: %s", strerror(errno));
        close(client->fd);
        client->fd = -1;
        return ERROR_GENERAL;
    }

    /* 清空缓冲区 */
    tcflush(client->fd, TCIOFLUSH);

    client->state = UART_STATE_OPEN;
    client->running = true;

    /* 设置线程属性，增加栈大小 */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);  /* 2MB栈 */

    /* 创建接收线程 */
    if (pthread_create(&client->recv_thread, &attr, uart_recv_thread, client) != 0) {
        LOG_ERROR("Failed to create UART recv thread");
        pthread_attr_destroy(&attr);
        close(client->fd);
        client->fd = -1;
        client->state = UART_STATE_CLOSED;
        return ERROR_GENERAL;
    }

    /* 创建轮询线程 */
    if (pthread_create(&client->poll_thread, &attr, uart_poll_thread, client) != 0) {
        LOG_ERROR("Failed to create UART poll thread");
        client->running = false;
        pthread_join(client->recv_thread, NULL);
        client->recv_thread = 0;   /* 清零：否则后续 close 会对同一 tid 二次 join */
        pthread_attr_destroy(&attr);
        close(client->fd);
        client->fd = -1;
        client->state = UART_STATE_CLOSED;
        return ERROR_GENERAL;
    }

    pthread_attr_destroy(&attr);

    LOG_INFO("UART device opened: %s (921600-8N1)", client->device);
    return SUCCESS;
}

static void* uart_recv_thread(void *arg)
{
    uart_client_t *client = (uart_client_t*)arg;
    uint8_t buffer[1024];
    static uint8_t frame_buffer[2048];  /* 使用静态缓冲区，避免栈溢出 */
    static uint32_t frame_offset = 0;
    int ret;

    /* 重置静态变量（防止第二次运行时残留数据） */
    frame_offset = 0;
    memset(frame_buffer, 0, sizeof(frame_buffer));

    LOG_INFO("UART recv thread started");

    while (client->running) {
        ret = read(client->fd, buffer, sizeof(buffer));
        if (ret > 0) {
            //LOG_DEBUG("UART received %d bytes", ret);

            /* 将数据追加到帧缓冲区 */
            if (frame_offset + ret > sizeof(frame_buffer)) {
                LOG_WARN("Frame buffer would overflow, resetting (offset=%u, new=%d, size=%zu)",
                         frame_offset, ret, sizeof(frame_buffer));
                frame_offset = 0;
            }

            /* 再次检查：即使重置后，单次接收的数据也不能超过缓冲区大小 */
            if (ret > sizeof(frame_buffer)) {
                LOG_ERROR("Single receive too large: %d bytes (buffer size: %zu), discarding",
                         ret, sizeof(frame_buffer));
                continue;
            }

            memcpy(frame_buffer + frame_offset, buffer, ret);
            frame_offset += ret;

            /* 处理缓冲区中的所有完整帧 */
            uint32_t processed_bytes = 0;
            while (processed_bytes < frame_offset) {
                uint8_t *buf_start = frame_buffer + processed_bytes;
                uint32_t remaining = frame_offset - processed_bytes;

                /* 搜索帧头。本 UART 上有两种帧混流且分帧规则不同：
                 *   0xEB90 状态帧    = 帧头(2) + 长度(2,大端) + 数据(127) + 帧尾(2)
                 *   0xEB93 遥测日志帧 = 帧头(2) + 来源(1) + 长度(1) + 数据(N) + 帧尾(2)
                 * 所以先扫出命中哪一种，再走各自的长度的解析。
                 *
                 * 扫描上界用 header_pos + 1 < remaining（等价于"还够读 2 字节"）：
                 * 原写法 header_pos < remaining - 1 在 remaining == 1 时循环体一次都不
                 * 执行，末尾那 1 个字节会被下面判为"找不到帧头"而整块丢弃 —— 若它恰好是
                 * 被 read() 切断的半个帧头(0xEB)，该帧就必丢。混流两种帧头后重新同步更
                 * 频繁，这个缺陷会被放大，故一并修正。 */
                uint32_t header_pos = 0;
                uint16_t frame_hdr = 0;
                bool found_header = false;
                for (header_pos = 0; header_pos + 1 < remaining; header_pos++) {
                    memcpy(&frame_hdr, buf_start + header_pos, 2);
                    frame_hdr = NTOHS(frame_hdr);
                    if (frame_hdr == FPGA_FRAME_HEADER ||
                        frame_hdr == FPGA_TELEMETRY_LOG_HEADER) {
                        found_header = true;
                        break;
                    }
                }

                if (!found_header) {
                    /* 没有找到帧头。通常整块丢弃；唯一例外是末尾那个字节等于帧头首字节
                     * (0xEB90/0xEB93 的首字节同为 0xEB)，它可能是被 read() 切断的半个
                     * 帧头，保留它等下一批数据拼回来。只保留这一种字节，是为了避免在长串
                     * 垃圾数据里每轮都留下 1 字节、把同一条 WARN 反复打出来。 */
                    if (remaining >= 1 &&
                        buf_start[remaining - 1] == (uint8_t)(FPGA_FRAME_HEADER >> 8)) {
                        processed_bytes += remaining - 1;
                    } else {
                        processed_bytes = frame_offset;
                    }
                    break;
                }

                /* 跳过无效数据 */
                if (header_pos > 0) {
                    LOG_WARN("Skipped %u invalid bytes before frame header", header_pos);
                }
                processed_bytes += header_pos;
                buf_start += header_pos;
                remaining -= header_pos;

                /* 两种帧都要够读偏移 2、3 两字节才能定位长度 */
                if (remaining < 4) {
                    break;  /* 等待更多数据 */
                }

                uint32_t expected_frame_len;
                uint16_t length_field = 0;   /* 仅 0xEB90 状态帧有效，分发时要用 */

                if (frame_hdr == FPGA_TELEMETRY_LOG_HEADER) {
                    /* 遥测日志帧：偏移2 = 来源，偏移3 = "有效数据长度"，总长 = 6 + N。
                     * 这里同时做来源白名单与长度交叉校验，理由见
                     * fpga_telemetry_payload_len() 的注释：帧头+帧尾两项校验太弱，
                     * 满速乱码下平均每分钟就能凑出一个假帧。
                     * 校验不过按非法帧处理，只丢 1 字节重新同步。 */
                    uint8_t  src      = buf_start[2];
                    uint8_t  data_len = buf_start[3];
                    uint32_t expect_len = fpga_telemetry_payload_len(src);

                    if (expect_len == 0 || data_len != expect_len) {
                        if (telemetry_reject_should_log()) {
                            /* 必须带上来源与实际长度：FPGA 若改了载荷长度，现场只表现为
                             * "遥测日志突然空了一半"，有这两个值才能一眼看出原因 */
                            LOG_WARN("Telemetry frame rejected: src=0x%02X len=%u "
                                     "(protocol len=%u), drop 1 byte",
                                     src, data_len, expect_len);
                        }
                        processed_bytes += 1;
                        continue;
                    }
                    expected_frame_len = 6 + data_len;
                } else {
                    /* 状态帧：偏移 2 起 2 字节大端长度（FPGA 实际发的是 0x00 0x7F） */
                    memcpy(&length_field, buf_start + 2, 2);
                    length_field = NTOHS(length_field);
                    expected_frame_len = 2 + 2 + length_field + 2;
                }

                /* 帧长度的通用合理性检查。
                 * 遥测分支上面已把长度钉在协议值上，这里实际只对状态帧生效。 */
                if (expected_frame_len < FPGA_MIN_FRAME_SIZE ||
                    expected_frame_len > sizeof(frame_buffer)) {
                    LOG_ERROR("Invalid frame length: %u (header=0x%04X), discarding 1 byte",
                              expected_frame_len, frame_hdr);
                    processed_bytes += 1;
                    continue;
                }

                /* 检查是否接收到完整帧 */
                if (remaining < expected_frame_len) {
                    break;  /* 等待更多数据 */
                }

                /* 验证帧尾 */
                uint16_t tail;
                memcpy(&tail, buf_start + expected_frame_len - 2, 2);
                tail = NTOHS(tail);

                if (tail != FPGA_FRAME_TAIL) {
                    LOG_WARN("Frame tail mismatch: 0x%04X, discarding 1 byte", tail);
                    processed_bytes += 1;
                    continue;
                }

                /* 处理完整帧。按帧头分流：遥测日志帧原样交给上层落盘；0xEB90 帧沿用
                 * 原有判定（133/127 → 状态帧，其余走 on_data_received）。
                 * 刻意不把 0xEB93 塞进下面的 else：那会把遥测帧交给 on_data_received，
                 * 而 main.c 的实现是空壳会直接把数据丢掉。 */
                if (frame_hdr == FPGA_TELEMETRY_LOG_HEADER) {
                    /* buf_start 指向本线程的 static frame_buffer，循环末尾的 memmove
                     * 会立刻覆盖它 —— 回调实现必须在返回前把整帧拷走，不能只存指针。
                     * （与 on_status_received 传 &static_status 是同一类约定。） */
                    if (client->on_telemetry_log_frame) {
                        client->on_telemetry_log_frame(buf_start, expected_frame_len);
                    }
                } else if (expected_frame_len == 133 && length_field == 127) {
                    /* 状态响应帧 */
                    if (expected_frame_len == sizeof(fpga_status_frame_t)) {
                        /* 使用静态缓冲区，避免栈溢出 */
                        static fpga_status_frame_t status __attribute__((aligned(8)));
                        memset(&status, 0, sizeof(status));

                        if (fpga_decode_status_frame(buf_start, expected_frame_len, &status) == SUCCESS) {
                            if (client && client->on_status_received) {
                                client->on_status_received(&status);
                            }
                        } else {
                            LOG_ERROR("Failed to decode FPGA status frame");
                        }
                    } else {
                        LOG_WARN("Invalid status frame length: %u (expected %zu)",
                                 expected_frame_len, sizeof(fpga_status_frame_t));
                    }
                } else {
                    /* 其他消息 */
                    //LOG_DEBUG("Other FPGA frame: length=%u", length_field);
                    if (client->on_data_received) {
                        client->on_data_received(buf_start, expected_frame_len);
                    }
                }

                processed_bytes += expected_frame_len;
            }

            /* 移除已处理的数据 */
            if (processed_bytes > 0) {
                if (processed_bytes >= frame_offset) {
                    /* 所有数据都已处理 */
                    frame_offset = 0;
                } else {
                    /* 还有未处理的数据，移动到缓冲区开头 */
                    uint32_t remaining_bytes = frame_offset - processed_bytes;

                    /* 边界检查 */
                    if (processed_bytes > sizeof(frame_buffer) ||
                        remaining_bytes > sizeof(frame_buffer) ||
                        processed_bytes + remaining_bytes > sizeof(frame_buffer)) {
                        LOG_ERROR("Buffer overflow detected: processed=%u, remaining=%u, buffer_size=%zu",
                                 processed_bytes, remaining_bytes, sizeof(frame_buffer));
                        frame_offset = 0;  /* 重置缓冲区 */
                    } else {
                        memmove(frame_buffer, frame_buffer + processed_bytes, remaining_bytes);
                        frame_offset = remaining_bytes;
                    }
                }
            }
        } else if (ret < 0 && errno != EAGAIN && errno != EINTR) {
            LOG_ERROR("UART read error: %s", strerror(errno));
            break;
        }

        usleep(10000);  /* 10ms */
    }

    LOG_INFO("UART recv thread exited");
    return NULL;
}

static void* uart_poll_thread(void *arg)
{
    uart_client_t *client = (uart_client_t*)arg;

    uint8_t payload = 0;

    LOG_INFO("UART poll thread started");
    
    while (client->running) {
        /* 每秒发送一次状态查询 */
        sleep(1);

        if (!client->running) {
            break;
        }

        /* 构造状态查询消息 */
        fpga_message_t msg;
        msg.msg_id = FPGA_MSG_STATUS_QUERY;
        msg.payload = &payload;
        msg.payload_len = 1;

        uint8_t buffer[16];
        int len = fpga_encode_message(&msg, buffer, sizeof(buffer));
        if (len > 0) {
            int ret = uart_client_send(client, buffer, len);
            if (ret == SUCCESS) {
                //LOG_DEBUG("Sent status query to FPGA");
            } else {
                LOG_ERROR("Failed to send status query: %d", ret);
            }
        }
    }

    LOG_INFO("UART poll thread exited");
    return NULL;
}

int uart_client_send(uart_client_t *client, const uint8_t *data, uint32_t len)
{
    if (!client || !data || len == 0) {
        return ERROR_INVALID_PARAM;
    }

    if (client->state != UART_STATE_OPEN || client->fd < 0) {
        LOG_ERROR("UART not open, cannot send");
        return ERROR_GENERAL;
    }

    pthread_mutex_lock(&client->send_mutex);

    uint32_t sent = 0;
    while (sent < len) {
        ssize_t ret = write(client->fd, data + sent, len - sent);
        if (ret < 0) {
            LOG_ERROR("UART write error: %s", strerror(errno));
            pthread_mutex_unlock(&client->send_mutex);
            return ERROR_GENERAL;
        }
        sent += (uint32_t)ret;
    }

    /* 等待数据发送完成 */
    tcdrain(client->fd);

    pthread_mutex_unlock(&client->send_mutex);

    //LOG_DEBUG("UART sent %d bytes", len);
    return SUCCESS;
}

uart_state_t uart_client_get_state(uart_client_t *client)
{
    if (!client) {
        return UART_STATE_ERROR;
    }
    return client->state;
}

int uart_client_close(uart_client_t *client)
{
    if (!client) {
        return ERROR_INVALID_PARAM;
    }

    client->running = false;

    if (client->fd >= 0) {
        close(client->fd);
        client->fd = -1;
    }
    client->state = UART_STATE_CLOSED;

    /* 等待线程退出；join 后清零 tid，使本函数可安全重复调用。
     * main.c 的清理顺序是 uart_client_close() → uart_client_destroy()，
     * 而 destroy 内部又会调一次 close —— 不清零就是对同一 tid 二次 join（UB）。 */
    if (client->recv_thread) {
        pthread_join(client->recv_thread, NULL);
        client->recv_thread = 0;
    }
    if (client->poll_thread) {
        pthread_join(client->poll_thread, NULL);
        client->poll_thread = 0;
    }

    LOG_INFO("UART device closed");
    return SUCCESS;
}

void uart_client_destroy(uart_client_t *client)
{
    if (!client) {
        return;
    }

    uart_client_close(client);
    pthread_mutex_destroy(&client->send_mutex);
    LOG_INFO("UART client destroyed");
}
