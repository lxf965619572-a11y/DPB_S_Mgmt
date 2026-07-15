#include "heartbeat.h"
#include "logger.h"
#include "tcp_client.h"
#include "channel_setup.h"
#include <time.h>

/* 全局心跳管理器 */
static heartbeat_manager_t g_heartbeat_mgr;

/* 外部TCP客户端引用 */
extern tcp_client_t g_tcp_client;

/* 外部流水号引用 */
extern uint32_t g_serial_num;
extern pthread_mutex_t g_serial_num_mutex;

/* 获取下一个流水号（线程安全） */
static inline uint32_t get_next_serial_num_safe(void)
{
    pthread_mutex_lock(&g_serial_num_mutex);
    uint32_t serial = ++g_serial_num;
    pthread_mutex_unlock(&g_serial_num_mutex);
    return serial;
}

int heartbeat_init(void)
{
    memset(&g_heartbeat_mgr, 0, sizeof(g_heartbeat_mgr));
    g_heartbeat_mgr.heartbeat_enabled = false;

    if (pthread_mutex_init(&g_heartbeat_mgr.mutex, NULL) != 0) {
        LOG_ERROR("Failed to init heartbeat mutex");
        return ERROR_GENERAL;
    }

    LOG_INFO("Heartbeat manager initialized");
    return SUCCESS;
}

int heartbeat_start(void)
{
    pthread_mutex_lock(&g_heartbeat_mgr.mutex);

    g_heartbeat_mgr.heartbeat_enabled = true;
    g_heartbeat_mgr.paau_heartbeat_count = 0;
    g_heartbeat_mgr.bbu_heartbeat_miss_count = 0;
    g_heartbeat_mgr.last_bbu_heartbeat_time = time(NULL);

    pthread_mutex_unlock(&g_heartbeat_mgr.mutex);

    LOG_INFO("Heartbeat started");
    return SUCCESS;
}

int heartbeat_stop(void)
{
    pthread_mutex_lock(&g_heartbeat_mgr.mutex);
    g_heartbeat_mgr.heartbeat_enabled = false;
    pthread_mutex_unlock(&g_heartbeat_mgr.mutex);

    LOG_INFO("Heartbeat stopped");
    return SUCCESS;
}

int heartbeat_send_paau(void)
{
    pthread_mutex_lock(&g_heartbeat_mgr.mutex);

    if (!g_heartbeat_mgr.heartbeat_enabled) {
        pthread_mutex_unlock(&g_heartbeat_mgr.mutex);
        return SUCCESS;
    }

    pthread_mutex_unlock(&g_heartbeat_mgr.mutex);

    /* 构造PAAU心跳消息（仅消息头，无payload） */
    cpri_message_t msg;
    memset(&msg, 0, sizeof(msg));

    msg.header.msg_id = MSG_PAAU_HEARTBEAT;
    msg.header.paau_id = 0;
    msg.header.bbu_id = 0;
    msg.header.port_num = 0;
    msg.header.serial_num = get_next_serial_num_safe();  /* 使用递增的流水号（线程安全） */
    msg.payload = NULL;
    msg.payload_len = 0;

    /* 编码并发送 */
    uint8_t buffer[32];
    int len = cpri_encode_message(&msg, buffer, sizeof(buffer));
    if (len > 0) {
        tcp_client_send(&g_tcp_client, buffer, len);

        pthread_mutex_lock(&g_heartbeat_mgr.mutex);
        g_heartbeat_mgr.paau_heartbeat_count++;
        pthread_mutex_unlock(&g_heartbeat_mgr.mutex);

        // LOG_DEBUG("Sent PAAU heartbeat (count=%u, serial_num=%u)",
        //          g_heartbeat_mgr.paau_heartbeat_count, msg.header.serial_num);
        return SUCCESS;
    }

    return ERROR_GENERAL;
}

int heartbeat_handle_bbu(const cpri_message_t *msg)
{
    if (!msg) {
        return ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&g_heartbeat_mgr.mutex);

    if (!g_heartbeat_mgr.heartbeat_enabled) {
        pthread_mutex_unlock(&g_heartbeat_mgr.mutex);
        return SUCCESS;
    }

    /* 更新BBU心跳时间 */
    g_heartbeat_mgr.last_bbu_heartbeat_time = time(NULL);
    g_heartbeat_mgr.bbu_heartbeat_miss_count = 0;

    pthread_mutex_unlock(&g_heartbeat_mgr.mutex);

    // LOG_DEBUG("Received BBU heartbeat");
    return SUCCESS;
}

bool heartbeat_check_timeout(void)
{
    pthread_mutex_lock(&g_heartbeat_mgr.mutex);

    if (!g_heartbeat_mgr.heartbeat_enabled) {
        pthread_mutex_unlock(&g_heartbeat_mgr.mutex);
        return false;
    }

    time_t now = time(NULL);
    time_t elapsed = now - g_heartbeat_mgr.last_bbu_heartbeat_time;

    /* 检查是否超过3秒未收到心跳 */
    if (elapsed >= HEARTBEAT_INTERVAL_SEC) {
        g_heartbeat_mgr.bbu_heartbeat_miss_count++;

        /* 检查是否达到超时阈值（20次，约60秒） */
        if (g_heartbeat_mgr.bbu_heartbeat_miss_count >= HEARTBEAT_TIMEOUT_COUNT) {
            LOG_ERROR("BBU heartbeat timeout (missed %u times, %ld seconds)",
                     g_heartbeat_mgr.bbu_heartbeat_miss_count, elapsed);
            pthread_mutex_unlock(&g_heartbeat_mgr.mutex);
            return true;
        }

        /* 更新时间基准，避免重复计数 */
        g_heartbeat_mgr.last_bbu_heartbeat_time = now;
    }

    pthread_mutex_unlock(&g_heartbeat_mgr.mutex);
    return false;
}

void heartbeat_destroy(void)
{
    pthread_mutex_destroy(&g_heartbeat_mgr.mutex);
    LOG_INFO("Heartbeat manager destroyed");
}
