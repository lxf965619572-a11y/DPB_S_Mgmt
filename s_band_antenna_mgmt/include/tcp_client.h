#ifndef TCP_CLIENT_H
#define TCP_CLIENT_H

#include "common.h"

/* TCP客户端状态 */
typedef enum {
    TCP_STATE_DISCONNECTED = 0,
    TCP_STATE_CONNECTING,
    TCP_STATE_CONNECTED,
    TCP_STATE_ERROR
} tcp_state_t;

/* TCP客户端上下文 */
typedef struct {
    int sockfd;
    tcp_state_t state;
    char server_ip[32];
    uint16_t server_port;
    pthread_t recv_thread;
    pthread_t reconnect_thread;
    bool running;
    pthread_mutex_t state_mutex;

    /* 回调函数 */
    void (*on_connected)(void);
    void (*on_disconnected)(void);
    void (*on_data_received)(const uint8_t *data, uint32_t len);
} tcp_client_t;

/* 函数声明 */
int tcp_client_init(tcp_client_t *client, const char *server_ip, uint16_t server_port);
int tcp_client_start(tcp_client_t *client);
int tcp_client_stop(tcp_client_t *client);
int tcp_client_send(tcp_client_t *client, const uint8_t *data, uint32_t len);
tcp_state_t tcp_client_get_state(tcp_client_t *client);
void tcp_client_destroy(tcp_client_t *client);

#endif /* TCP_CLIENT_H */
