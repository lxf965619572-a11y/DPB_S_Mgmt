#ifndef UART_CLIENT_H
#define UART_CLIENT_H

#include "common.h"

/* UART配置参数 */
#define UART_DEVICE         "/dev/ttyAMA0"
#define UART_BAUDRATE       B921600
#define UART_DATABITS       8
#define UART_STOPBITS       1
#define UART_PARITY         'N'  /* N=None, E=Even, O=Odd */

/* UART客户端状态 */
typedef enum {
    UART_STATE_CLOSED = 0,
    UART_STATE_OPEN,
    UART_STATE_ERROR
} uart_state_t;

/* UART客户端上下文 */
typedef struct {
    int fd;
    uart_state_t state;
    char device[64];
    pthread_t recv_thread;
    pthread_t poll_thread;
    bool running;
    pthread_mutex_t send_mutex;

    /* 回调函数 */
    void (*on_data_received)(const uint8_t *data, uint32_t len);
    void (*on_status_received)(const void *status);
} uart_client_t;

/* 函数声明 */
int uart_client_init(uart_client_t *client, const char *device);
int uart_client_open(uart_client_t *client);
int uart_client_close(uart_client_t *client);
int uart_client_send(uart_client_t *client, const uint8_t *data, uint32_t len);
uart_state_t uart_client_get_state(uart_client_t *client);
void uart_client_destroy(uart_client_t *client);

#endif /* UART_CLIENT_H */
