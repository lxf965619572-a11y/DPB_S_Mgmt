#include <termios.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "uart_client.h"
#include "fpga_protocol.h"
#include "logger.h"

static void* uart_recv_thread(void *arg);
static void* uart_poll_thread(void *arg);

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

                /* 搜索帧头 */
                uint32_t header_pos = 0;
                bool found_header = false;
                for (header_pos = 0; header_pos < remaining - 1; header_pos++) {
                    uint16_t header;
                    memcpy(&header, buf_start + header_pos, 2);
                    header = NTOHS(header);
                    if (header == FPGA_FRAME_HEADER) {
                        found_header = true;
                        break;
                    }
                }

                if (!found_header) {
                    /* 没有找到帧头，丢弃所有数据 */
                    processed_bytes = frame_offset;
                    break;
                }

                /* 跳过无效数据 */
                if (header_pos > 0) {
                    LOG_WARN("Skipped %u invalid bytes before frame header", header_pos);
                }
                processed_bytes += header_pos;
                buf_start += header_pos;
                remaining -= header_pos;

                /* 检查是否有足够数据读取长度字段 */
                if (remaining < 4) {
                    break;  /* 等待更多数据 */
                }

                /* 读取长度字段（2字节，大端序） */
                uint16_t length_field;
                memcpy(&length_field, buf_start + 2, 2);
                length_field = NTOHS(length_field);

                /* 计算完整帧长度 */
                uint32_t expected_frame_len = 2 + 2 + length_field + 2;

                /* 验证帧长度的合理性 */
                if (expected_frame_len < 6 || expected_frame_len > sizeof(frame_buffer)) {
                    LOG_ERROR("Invalid frame length: %u, discarding 1 byte", expected_frame_len);
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

                /* 处理完整帧 */
                if (expected_frame_len == 133 && length_field == 127) {
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

    /* 等待线程退出 */
    if (client->recv_thread) {
        pthread_join(client->recv_thread, NULL);
    }
    if (client->poll_thread) {
        pthread_join(client->poll_thread, NULL);
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
