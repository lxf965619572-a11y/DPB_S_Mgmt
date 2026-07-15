/**
 * @file uart_rs422_client.h
 * @brief UART RS-422客户端 - 专用于FPGA固件上注(ttyAMA2)
 */

#ifndef UART_RS422_CLIENT_H
#define UART_RS422_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "common.h"


/* UART RS-422客户端结构 */
typedef struct {
    int fd;                         /* 文件描述符 */
    char device_path[64];           /* 设备路径 */
    bool is_open;                   /* 是否已打开 */
    pthread_mutex_t send_mutex;     /* 发送互斥锁 */
    pthread_mutex_t recv_mutex;     /* 接收互斥锁 */
    uint32_t timeout_ms;            /* 超时时间(毫秒) */
    uint32_t max_retries;           /* 最大重试次数 */
} uart_rs422_client_t;

/**
 * @brief 初始化UART RS-422客户端
 * @param client 客户端结构
 * @param device_path 设备路径(如"/dev/ttyAMA2")
 * @return 成功返回SUCCESS,失败返回错误码
 */
int uart_rs422_init(uart_rs422_client_t *client, const char *device_path);

/**
 * @brief 打开UART设备
 * @param client 客户端结构
 * @return 成功返回SUCCESS,失败返回错误码
 */
int uart_rs422_open(uart_rs422_client_t *client);

/**
 * @brief 关闭UART设备
 * @param client 客户端结构
 */
void uart_rs422_close(uart_rs422_client_t *client);

/**
 * @brief 发送帧(带重试)
 * @param client 客户端结构
 * @param frame 帧数据
 * @param frame_len 帧长度
 * @return 成功返回SUCCESS,失败返回错误码
 */
int uart_rs422_send_frame(uart_rs422_client_t *client,
                           const uint8_t *frame, uint32_t frame_len);

/**
 * @brief 接收帧(带超时)
 * @param client 客户端结构
 * @param frame_buf 接收缓冲区
 * @param frame_buf_size 缓冲区大小
 * @param received_len 输出:实际接收长度
 * @return 成功返回SUCCESS,超时返回ERROR_TIMEOUT,失败返回错误码
 */
int uart_rs422_recv_frame(uart_rs422_client_t *client,
                           uint8_t *frame_buf, uint32_t frame_buf_size,
                           uint32_t *received_len);

/**
 * @brief 发送帧并等待响应
 * @param client 客户端结构
 * @param send_frame 发送帧
 * @param send_len 发送长度
 * @param recv_frame 接收缓冲区
 * @param recv_buf_size 接收缓冲区大小
 * @param recv_len 输出:实际接收长度
 * @param expected_cmd 期望的响应命令码(0表示不检查)
 * @return 成功返回SUCCESS,失败返回错误码
 */
int uart_rs422_send_and_wait(uart_rs422_client_t *client,
                              const uint8_t *send_frame, uint32_t send_len,
                              uint8_t *recv_frame, uint32_t recv_buf_size,
                              uint32_t *recv_len, uint16_t expected_cmd);

/**
 * @brief 设置超时时间
 * @param client 客户端结构
 * @param timeout_ms 超时时间(毫秒)
 */
void uart_rs422_set_timeout(uart_rs422_client_t *client, uint32_t timeout_ms);

/**
 * @brief 设置最大重试次数
 * @param client 客户端结构
 * @param max_retries 最大重试次数
 */
void uart_rs422_set_max_retries(uart_rs422_client_t *client, uint32_t max_retries);

#endif /* UART_RS422_CLIENT_H */
