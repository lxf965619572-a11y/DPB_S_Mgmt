#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "tcp_client.h"
#include "alarm_manager.h"
#include "logger.h"

static void* tcp_recv_thread(void *arg);
static void* tcp_reconnect_thread(void *arg);
static int tcp_connect_to_server(tcp_client_t *client);

int tcp_client_init(tcp_client_t *client, const char *server_ip, uint16_t server_port)
{
    if (!client || !server_ip) {
        return ERROR_INVALID_PARAM;
    }

    memset(client, 0, sizeof(tcp_client_t));
    strncpy(client->server_ip, server_ip, sizeof(client->server_ip) - 1);
    client->server_port = server_port;
    client->sockfd = -1;
    client->state = TCP_STATE_DISCONNECTED;
    client->running = false;

    if (pthread_mutex_init(&client->state_mutex, NULL) != 0) {
        LOG_ERROR("Failed to init mutex");
        return ERROR_GENERAL;
    }

    LOG_INFO("TCP client initialized: %s:%d", server_ip, server_port);
    return SUCCESS;
}

int tcp_client_start(tcp_client_t *client)
{
    if (!client) {
        return ERROR_INVALID_PARAM;
    }

    client->running = true;

    /* 创建重连线程 */
    if (pthread_create(&client->reconnect_thread, NULL, tcp_reconnect_thread, client) != 0) {
        LOG_ERROR("Failed to create reconnect thread");
        client->running = false;
        return ERROR_GENERAL;
    }

    LOG_INFO("TCP client started");
    return SUCCESS;
}

static int tcp_connect_to_server(tcp_client_t *client)
{
    struct sockaddr_in server_addr;
    int sockfd;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        LOG_ERROR("Failed to create socket: %s", strerror(errno));
        return ERROR_NETWORK;
    }

    /* 设置非阻塞模式 */
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(client->server_port);
    inet_pton(AF_INET, client->server_ip, &server_addr.sin_addr);

    LOG_INFO("Connecting to BBU server %s:%d...", client->server_ip, client->server_port);

    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        if (errno != EINPROGRESS) {
            LOG_ERROR("Connect failed: %s", strerror(errno));
            close(sockfd);
            return ERROR_NETWORK;
        }

        /* 等待连接完成 */
        fd_set wfds;
        struct timeval tv;
        FD_ZERO(&wfds);
        FD_SET(sockfd, &wfds);
        tv.tv_sec = 5;
        tv.tv_usec = 0;

        int ret = select(sockfd + 1, NULL, &wfds, NULL, &tv);
        if (ret <= 0) {
            LOG_ERROR("Connect timeout or error");
            close(sockfd);
            return ERROR_TIMEOUT;
        }

        int error = 0;
        socklen_t len = sizeof(error);
        getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len);
        if (error != 0) {
            LOG_ERROR("Connect failed: %s", strerror(error));
            close(sockfd);
            return ERROR_NETWORK;
        }
    }

    /* 恢复阻塞模式 */
    fcntl(sockfd, F_SETFL, flags);

    pthread_mutex_lock(&client->state_mutex);
    client->sockfd = sockfd;
    client->state = TCP_STATE_CONNECTED;
    pthread_mutex_unlock(&client->state_mutex);

    LOG_INFO("Connected to BBU server successfully");

    /* 设置TCP连接状态并清除TCP断开告警 */
    alarm_set_tcp_status(true);
    alarm_clear_tcp_disconnect();

    /* 创建接收线程 */
    if (pthread_create(&client->recv_thread, NULL, tcp_recv_thread, client) != 0) {
        LOG_ERROR("Failed to create recv thread");
        close(sockfd);
        client->sockfd = -1;
        client->state = TCP_STATE_DISCONNECTED;
        return ERROR_GENERAL;
    }

    /* 调用连接成功回调 */
    if (client->on_connected) {
        client->on_connected();
    }

    return SUCCESS;
}

static void* tcp_reconnect_thread(void *arg)
{
    tcp_client_t *client = (tcp_client_t*)arg;

    while (client->running) {
        pthread_mutex_lock(&client->state_mutex);
        tcp_state_t state = client->state;
        pthread_mutex_unlock(&client->state_mutex);

        if (state == TCP_STATE_DISCONNECTED) {
            if (tcp_connect_to_server(client) != SUCCESS) {
                LOG_WARN("Reconnect failed, retry after %d ms", g_config.reconnect_interval_ms);
                usleep(g_config.reconnect_interval_ms * 1000);
            }
        } else {
            sleep(1);
        }
    }

    return NULL;
}

static void* tcp_recv_thread(void *arg)
{
    tcp_client_t *client = (tcp_client_t*)arg;
    uint8_t buffer[8192];
    int ret;

    while (client->running) {
        ret = recv(client->sockfd, buffer, sizeof(buffer), 0);
        if (ret > 0) {
            LOG_DEBUG("Received %d bytes from BBU", ret);
            if (client->on_data_received) {
                client->on_data_received(buffer, ret);
            }
        } else if (ret == 0) {
            LOG_WARN("Connection closed by BBU (recv returned 0 - FIN received)");
            LOG_WARN("  sockfd=%d, client->running=%d", client->sockfd, client->running);
            break;
        } else {
            if (errno == EINTR) {
                LOG_DEBUG("recv() interrupted by signal, retrying");
                continue;
            }
            LOG_ERROR("Recv error: %s (errno=%d)", strerror(errno), errno);
            LOG_ERROR("  sockfd=%d, client->running=%d", client->sockfd, client->running);
            break;
        }
    }

    LOG_INFO("Recv thread exiting: client->running=%d, last_ret=%d", client->running, ret);

    /* 连接断开处理 */
    pthread_mutex_lock(&client->state_mutex);
    if (client->sockfd >= 0) {
        close(client->sockfd);
        client->sockfd = -1;
    }
    client->state = TCP_STATE_DISCONNECTED;
    pthread_mutex_unlock(&client->state_mutex);

    /* 设置TCP断开状态并触发告警 */
    alarm_set_tcp_status(false);
    alarm_trigger_tcp_disconnect("Connection lost");

    if (client->on_disconnected) {
        client->on_disconnected();
    }

    LOG_INFO("Recv thread exited");
    return NULL;
}

int tcp_client_send(tcp_client_t *client, const uint8_t *data, uint32_t len)
{
    if (!client || !data || len == 0) {
        return ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&client->state_mutex);
    if (client->state != TCP_STATE_CONNECTED || client->sockfd < 0) {
        pthread_mutex_unlock(&client->state_mutex);
        LOG_ERROR("TCP not connected, cannot send");
        return ERROR_NETWORK;
    }

    int sockfd = client->sockfd;
    pthread_mutex_unlock(&client->state_mutex);

    uint32_t sent = 0;
    while (sent < len) {
        ssize_t ret = send(sockfd, data + sent, len - sent, 0);
        if (ret < 0) {
            LOG_ERROR("Send error: %s", strerror(errno));
            return ERROR_NETWORK;
        }
        sent += (uint32_t)ret;
    }

    // LOG_DEBUG("Sent %d bytes to BBU", len);
    return SUCCESS;
}

tcp_state_t tcp_client_get_state(tcp_client_t *client)
{
    if (!client) {
        return TCP_STATE_ERROR;
    }

    pthread_mutex_lock(&client->state_mutex);
    tcp_state_t state = client->state;
    pthread_mutex_unlock(&client->state_mutex);

    return state;
}

int tcp_client_stop(tcp_client_t *client)
{
    if (!client) {
        return ERROR_INVALID_PARAM;
    }

    client->running = false;

    pthread_mutex_lock(&client->state_mutex);
    if (client->sockfd >= 0) {
        close(client->sockfd);
        client->sockfd = -1;
    }
    client->state = TCP_STATE_DISCONNECTED;
    pthread_mutex_unlock(&client->state_mutex);

    /* 等待线程退出 */
    if (client->recv_thread) {
        pthread_join(client->recv_thread, NULL);
    }
    if (client->reconnect_thread) {
        pthread_join(client->reconnect_thread, NULL);
    }

    LOG_INFO("TCP client stopped");
    return SUCCESS;
}

void tcp_client_destroy(tcp_client_t *client)
{
    if (!client) {
        return;
    }

    tcp_client_stop(client);
    pthread_mutex_destroy(&client->state_mutex);
    LOG_INFO("TCP client destroyed");
}
