#include "uart_rs422_client.h"
#include "rs422_protocol.h"
#include "logger.h"
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/select.h>
#include <errno.h>
#include <arpa/inet.h>
#include "rBuf.h"
#include "pkfinder.h"


/* 默认配置 */
#define DEFAULT_TIMEOUT_MS      5000    /* 默认超时5秒 */
#define DEFAULT_MAX_RETRIES     3       /* 默认最大重试3次 */
#define BAUD_RATE               B921600 /* 波特率921600 */

/* 接收上下文结构体 - 封装所有接收相关状态 */
typedef struct {
    uint8_t rd_buf[1024];           /* 临时读取缓冲区 */
    uint8_t rbuf_storage[5120];     /* 环形缓冲区存储 */
    RBUF rbuf;                      /* 环形缓冲区 */
    uint8_t parse_buf[5120];        /* 解析缓冲区 */
    uint8_t received_frame[2048];   /* 完整包缓存 */
    uint32_t received_frame_len;    /* 完整包长度 */
    int frame_ready;                /* 包就绪标志: 0=未就绪, 1=已就绪 */
    PKFINDER pkfinder;              /* 包查找器 */
    uint8_t head[2];                /* 包头 */
    uint8_t head_mask[2];           /* 包头掩码 */
} uart_recv_context_t;

/* 全局接收上下文 */
static uart_recv_context_t g_recv_ctx;

/**
 * @brief 包校验和处理回调函数
 * @param pkBuf 包缓存地址
 * @param pkLen 包长度
 * @return TRUE(gtypes.h定义=0)表示校验通过，FALSE(gtypes.h定义=-1)表示校验失败
 */
static BOOL rs422_frame_check_and_process(UCHAR* pkBuf, UINT pkLen)
{
    if (!pkBuf || pkLen == 0) {
        return FALSE;
    }

    /* 验证包长度是否合理 */
    if (pkLen < RS422_HEADER_SIZE + RS422_CHECKSUM_SIZE) {
        LOG_WARN("接收到的包长度太短: %u字节", pkLen);
        return FALSE;
    }

    if (pkLen > sizeof(g_recv_ctx.received_frame)) {
        LOG_ERROR("接收到的包长度超过缓冲区: %u字节", pkLen);
        return FALSE;
    }

    /* 验证校验和 */
    uint32_t checksum_offset = pkLen - RS422_CHECKSUM_SIZE;
    uint16_t expected_checksum = rs422_checksum(pkBuf + 2, checksum_offset - 2);
    uint16_t expected_checksum_be = htons(expected_checksum);
    uint16_t actual_checksum_be;
    memcpy(&actual_checksum_be, pkBuf + checksum_offset, 2);

    if (actual_checksum_be != expected_checksum_be) {
        /* 打印完整包数据用于调试 */
        char hex_str[256] = {0};
        int print_len = (pkLen > 64) ? 64 : pkLen;
        for (int i = 0; i < print_len; i++) {
            sprintf(hex_str + i*3, "%02X ", pkBuf[i]);
        }
        LOG_WARN("校验和不匹配: 实际 0x%04X, 期望 0x%04X, 包长度=%u",
                 ntohs(actual_checksum_be), expected_checksum, pkLen);
        LOG_WARN("完整包数据: %s%s", hex_str, (pkLen > 64) ? "..." : "");
        return FALSE;
    }

    /* 校验通过，保存完整包到上下文 */
    memcpy(g_recv_ctx.received_frame, pkBuf, pkLen);
    g_recv_ctx.received_frame_len = pkLen;
    g_recv_ctx.frame_ready = 1;

    return TRUE;
}

/**
 * @brief 初始化接收上下文
 */
static void uart_recv_context_init(uart_recv_context_t *ctx)
{
    if (!ctx) return;

    memset(ctx, 0, sizeof(uart_recv_context_t));

    /* 初始化环形缓冲区 */
    ctx->rbuf.buf = ctx->rbuf_storage;
    ctx->rbuf.len = sizeof(ctx->rbuf_storage);
    ctx->rbuf.inIndex = 0;
    ctx->rbuf.outIndex = 0;
    rBufInit(&ctx->rbuf);

    /* 初始化包头 */
    ctx->head[0] = 0x1a;
    ctx->head[1] = 0xcf;
    ctx->head_mask[0] = 0xff;
    ctx->head_mask[1] = 0xff;

    /* 初始化PKFINDER
     * - headBuf: 包头 0x1ACF (应答帧)
     * - headLen: 2字节
     * - isPkFixLen: FALSE (变长包)
     * - pkLenPos: 6 (数据长度字段在第6字节)
     * - pkLenLen: 2 (长度字段占2字节)
     * - pkLenEnd: ENDIAN_BIG (大端序)
     * - pkLenAdd: 11 (包长=长度域值+11)
     */
    ctx->pkfinder.headBuf = ctx->head;
    ctx->pkfinder.headLen = 2;
    ctx->pkfinder.headMasks = ctx->head_mask;
    ctx->pkfinder.isPkFixLen = FALSE;
    ctx->pkfinder.pkFixLen = 0;
    ctx->pkfinder.pkLenPos = 6;
    ctx->pkfinder.pkLenLen = 2;
    ctx->pkfinder.pkLenFirstMsk = 0xff;
    ctx->pkfinder.pkLenEnd = ENDIAN_BIG;
    ctx->pkfinder.pkLenAdd = 11;
}
 

int uart_rs422_init(uart_rs422_client_t *client, const char *device_path)
{
    if (!client || !device_path) {
        return ERROR_INVALID_PARAM;
    }

    memset(client, 0, sizeof(uart_rs422_client_t));
    strncpy(client->device_path, device_path, sizeof(client->device_path) - 1);
    client->fd = -1;
    client->is_open = false;
    client->timeout_ms = DEFAULT_TIMEOUT_MS;
    client->max_retries = DEFAULT_MAX_RETRIES;

    pthread_mutex_init(&client->send_mutex, NULL);
    pthread_mutex_init(&client->recv_mutex, NULL);

    /* 初始化接收上下文 */
    uart_recv_context_init(&g_recv_ctx);

    LOG_INFO("UART RS-422客户端已初始化: %s", device_path);
    return SUCCESS;
}

int uart_rs422_open(uart_rs422_client_t *client)
{
    if (!client) {
        return ERROR_INVALID_PARAM;
    }

    if (client->is_open) {
        LOG_WARN("UART设备已打开");
        return SUCCESS;
    }

    /* 打开串口设备 */
    client->fd = open(client->device_path, O_RDWR | O_NOCTTY | O_NDELAY);
    if (client->fd < 0) {
        LOG_ERROR("无法打开UART设备: %s, errno=%d", client->device_path, errno);
        return ERROR_GENERAL;
    }

    /* 配置串口参数 */
    struct termios options;
    if (tcgetattr(client->fd, &options) != 0) {
        LOG_ERROR("获取串口属性失败");
        close(client->fd);
        client->fd = -1;
        return ERROR_GENERAL;
    }

    /* 设置波特率 */
    cfsetispeed(&options, BAUD_RATE);
    cfsetospeed(&options, BAUD_RATE);

    /* 检查是否是ttyAMA2（需要奇校验） */
    bool use_parity = (strstr(client->device_path, "ttyAMA2") != NULL);
    if (use_parity) {
        /* ttyAMA2: 8位数据位 + 奇校验 + 1个停止位 (8O1) */
        options.c_cflag |= PARENB;          /* 使能奇偶校验 */
        options.c_cflag |= PARODD;          /* 奇校验 */
        options.c_cflag &= ~CSTOPB;         /* 1个停止位 */
        options.c_cflag &= ~CSIZE;
        options.c_cflag |= CS8;             /* 8位数据位 */
        LOG_INFO("配置串口为8O1模式（8位数据+奇校验+1停止位）");
    } else {
        /* 其他串口: 8N1（8位数据+无校验+1停止位） */
        options.c_cflag &= ~PARENB;         /* 无奇偶校验 */
        options.c_cflag &= ~CSTOPB;         /* 1个停止位 */
        options.c_cflag &= ~CSIZE;
        options.c_cflag |= CS8;             /* 8位数据位 */
        LOG_INFO("配置串口为8N1模式（8位数据+无校验+1停止位）");
    }

    options.c_cflag |= (CLOCAL | CREAD); /* 本地连接,使能接收 */
    options.c_cflag &= ~CRTSCTS;        /* 无硬件流控 */

    /* 原始模式 */
    options.c_lflag &=(~(ICANON|ECHO|ECHOE|ECHOK|ECHONL|ISIG));
    options.c_iflag &= ~(IXON | IXOFF | IXANY);        /*无软件流控*/
    // options.c_iflag &= ~(INLCR | ICRNL | IGNCR);
    options.c_iflag &= (~(INPCK|IGNPAR|PARMRK|ISTRIP|ICRNL|IXANY));

    if (use_parity) {
        /* 奇校验模式：禁用输入校验检查和剥离 */
        options.c_iflag &= ~(INPCK | ISTRIP);
    }

    options.c_oflag &= ~OPOST;          /* 原始输出 */

    /* 设置超时 */
    options.c_cc[VTIME] = 0.1;            /* 使用select()超时 */
    options.c_cc[VMIN] = 1;

    /* 应用配置 */
    if (tcsetattr(client->fd, TCSANOW, &options) != 0) {
        LOG_ERROR("设置串口属性失败");
        close(client->fd);
        client->fd = -1;
        return ERROR_GENERAL;
    }

    /* 清空缓冲区 */
    tcflush(client->fd, TCIOFLUSH);

    client->is_open = true;

    if (strstr(client->device_path, "ttyAMA2") != NULL) {
        LOG_INFO("UART RS-422设备已打开: %s (波特率: 921600, 8O1-奇校验)", client->device_path);
    } else {
        LOG_INFO("UART RS-422设备已打开: %s (波特率: 921600, 8N1)", client->device_path);
    }

    return SUCCESS;
}

void uart_rs422_close(uart_rs422_client_t *client)
{
    if (!client || !client->is_open) {
        return;
    }

    if (client->fd >= 0) {
        close(client->fd);
        client->fd = -1;
    }

    client->is_open = false;
    pthread_mutex_destroy(&client->send_mutex);
    pthread_mutex_destroy(&client->recv_mutex);

    LOG_INFO("UART RS-422设备已关闭: %s", client->device_path);
}

int uart_rs422_send_frame(uart_rs422_client_t *client,
                           const uint8_t *frame, uint32_t frame_len)
{
    if (!client || !client->is_open || !frame || frame_len == 0) {
        return ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&client->send_mutex);

    uint32_t retry = 0;
    int ret = ERROR_GENERAL;

    while (retry <= client->max_retries) {
        ssize_t written = write(client->fd, frame, frame_len);

        if (written == (ssize_t)frame_len) {
            ret = SUCCESS;
            break;
        }

        if (written < 0) {
            LOG_ERROR("UART发送失败(尝试 %u/%u): errno=%d (%s)",
                     retry + 1, client->max_retries + 1, errno, strerror(errno));
        } else {
            LOG_WARN("UART部分发送(尝试 %u/%u): written=%ld, expected=%u",
                     retry + 1, client->max_retries + 1, written, frame_len);
        }

        retry++;

        if (retry <= client->max_retries) {
            usleep(100000);  /* 延迟100ms后重试 */
        }
    }

    pthread_mutex_unlock(&client->send_mutex);

    if (ret != SUCCESS) {
        LOG_ERROR("UART发送最终失败,已重试 %u 次", client->max_retries);
    }

    return ret;
}

int uart_rs422_recv_frame(uart_rs422_client_t *client,
                           uint8_t *frame_buf, uint32_t frame_buf_size,
                           uint32_t *received_len)
{
    if (!client || !client->is_open || !frame_buf || !received_len) {
        return ERROR_INVALID_PARAM;
    }

    *received_len = 0;

    /* 获取接收锁 */
    pthread_mutex_lock(&client->recv_mutex);

    /* 重置帧就绪标志 */
    g_recv_ctx.frame_ready = 0;

    /* 使用单调时钟计算精确超时 */
    struct timespec start_time, current_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    /* 循环接收，直到找到完整包或超时 */
    while (1) {
        /* 计算已消耗时间 */
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        uint32_t elapsed_ms = (current_time.tv_sec - start_time.tv_sec) * 1000 +
                              (current_time.tv_nsec - start_time.tv_nsec) / 1000000;

        if (elapsed_ms >= client->timeout_ms) {
            /* 超时 */
            uint32_t rbuf_len = rBufNBytes(&g_recv_ctx.rbuf);
            if (rbuf_len > 0) {
                LOG_WARN("UART接收超时但缓冲区有%u字节数据，可能包不完整", rbuf_len);
            } else {
                LOG_WARN("UART接收超时，缓冲区为空，FPGA可能未应答或应答丢失");
            }
            pthread_mutex_unlock(&client->recv_mutex);
            return ERROR_TIMEOUT;
        }

        /* 计算剩余超时时间 */
        uint32_t remaining_ms = client->timeout_ms - elapsed_ms;

        /* 使用select()等待数据 */
        fd_set read_fds;
        struct timeval timeout;

        FD_ZERO(&read_fds);
        FD_SET(client->fd, &read_fds);

        timeout.tv_sec = remaining_ms / 1000;
        timeout.tv_usec = (remaining_ms % 1000) * 1000;

        int select_ret = select(client->fd + 1, &read_fds, NULL, NULL, &timeout);

        if (select_ret < 0) {
            LOG_ERROR("select()失败: errno=%d (%s)", errno, strerror(errno));
            pthread_mutex_unlock(&client->recv_mutex);
            return ERROR_GENERAL;
        }

        if (select_ret == 0) {
            /* select超时，继续循环检查总超时 */
            continue;
        }

        /* 读取数据到临时缓冲区 */
        ssize_t ret = read(client->fd, g_recv_ctx.rd_buf, sizeof(g_recv_ctx.rd_buf));
        if (ret < 0) {
            LOG_ERROR("UART读取失败: errno=%d (%s)", errno, strerror(errno));
            pthread_mutex_unlock(&client->recv_mutex);
            return ERROR_GENERAL;
        }

        if (ret > 0) {
            /* 将数据放入环形缓冲区 - 增强容错 */
            uint32_t put_len = rBufPut(&g_recv_ctx.rbuf, g_recv_ctx.rd_buf, ret);
            if (put_len != (uint32_t)ret) {
                LOG_WARN("环形缓冲区空间不足: 期望写入%ld, 实际写入%u, 丢弃%ld字节",
                         ret, put_len, ret - put_len);
                /* 缓冲区满时，清空旧数据为新数据腾出空间 */
                if (put_len == 0) {
                    LOG_WARN("环形缓冲区已满，清空并重新写入");
                    rBufClear(&g_recv_ctx.rbuf);
                    put_len = rBufPut(&g_recv_ctx.rbuf, g_recv_ctx.rd_buf, ret);
                }
            }

            /* 解析环形队列，查找并处理完整包 */
            STATUS status = pkfinderParse(&g_recv_ctx.pkfinder, &g_recv_ctx.rbuf,
                                          g_recv_ctx.parse_buf, sizeof(g_recv_ctx.parse_buf),
                                          rs422_frame_check_and_process);

            /* 检查是否找到完整包 */
            if (status == OK && g_recv_ctx.frame_ready == 1) {
                /* 找到并校验通过的完整包 */
                if (g_recv_ctx.received_frame_len > frame_buf_size) {
                    LOG_ERROR("接收缓冲区太小: 需要%u字节, 提供%u字节",
                             g_recv_ctx.received_frame_len, frame_buf_size);
                    g_recv_ctx.frame_ready = 0;
                    pthread_mutex_unlock(&client->recv_mutex);
                    return ERROR_GENERAL;
                }

                /* 复制完整包到输出缓冲区 */
                memcpy(frame_buf, g_recv_ctx.received_frame, g_recv_ctx.received_frame_len);
                *received_len = g_recv_ctx.received_frame_len;
                g_recv_ctx.frame_ready = 0;

                pthread_mutex_unlock(&client->recv_mutex);
                return SUCCESS;
            } else if (status == ERROR) {
                /* 解析错误 */
                LOG_ERROR("包解析错误: pkfinderParse返回ERROR");
                pthread_mutex_unlock(&client->recv_mutex);
                return ERROR_GENERAL;
            }
            /* 否则继续接收 (status == OK 但 frame_ready == 0: 未找到包头或包不完整) */
        }
    }

    /* 不应到达此处 */
    pthread_mutex_unlock(&client->recv_mutex);
    return ERROR_TIMEOUT;
}

int uart_rs422_send_and_wait(uart_rs422_client_t *client,
                              const uint8_t *send_frame, uint32_t send_len,
                              uint8_t *recv_frame, uint32_t recv_buf_size,
                              uint32_t *recv_len, uint16_t expected_cmd)
{
    if (!client || !send_frame || !recv_frame || !recv_len) {
        return ERROR_INVALID_PARAM;
    }

    /* 发送帧 */
    int ret = uart_rs422_send_frame(client, send_frame, send_len);
    if (ret != SUCCESS) {
        LOG_WARN("uart_rs422_send_and_wait: 发送帧失败, 错误码=%d", ret);
        return ret;
    }

    /* 接收响应 */
    ret = uart_rs422_recv_frame(client, recv_frame, recv_buf_size, recv_len);
    if (ret != SUCCESS) {
        LOG_WARN("uart_rs422_send_and_wait: 接收响应失败, 错误码=%d", ret);
        return ret;
    }

    /* 如果指定了期望的命令码,进行验证 */
    if (expected_cmd != 0) {
        uint16_t apid, cmd_code;
        const uint8_t *payload;
        uint32_t payload_len;

        ret = rs422_decode_frame(recv_frame, *recv_len,
                                 &apid, &cmd_code, &payload, &payload_len);
        if (ret != SUCCESS) {
            LOG_ERROR("解码响应帧失败");
            return ret;
        }

        if (cmd_code != expected_cmd) {
            LOG_ERROR("响应命令码不匹配: 期望=0x%04X, 实际=0x%04X",
                      expected_cmd, cmd_code);
            return ERROR_GENERAL;
        }
    }

    return SUCCESS;
}

void uart_rs422_set_timeout(uart_rs422_client_t *client, uint32_t timeout_ms)
{
    if (client) {
        client->timeout_ms = timeout_ms;
        LOG_DEBUG("UART超时设置为: %u ms", timeout_ms);
    }
}

void uart_rs422_set_max_retries(uart_rs422_client_t *client, uint32_t max_retries)
{
    if (client) {
        client->max_retries = max_retries;
        LOG_DEBUG("UART最大重试次数设置为: %u", max_retries);
    }
}
