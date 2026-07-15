#include "cpri_protocol.h"
#include "logger.h"
#include <pthread.h>

/* 静态内存池配置 */
#define MAX_CPRI_PAYLOAD_SIZE 8192
#define MAX_CPRI_MESSAGE_POOL 16

/* 静态内存池 */
typedef struct {
    uint8_t in_use;
    uint8_t data[MAX_CPRI_PAYLOAD_SIZE];
} cpri_payload_buffer_t;

static cpri_payload_buffer_t g_cpri_payload_pool[MAX_CPRI_MESSAGE_POOL];
static pthread_mutex_t g_cpri_pool_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 从池中分配payload缓冲区 */
static uint8_t* cpri_payload_alloc(uint32_t size)
{
    if (size > MAX_CPRI_PAYLOAD_SIZE) {
        LOG_ERROR("Payload size %u exceeds max %u", size, MAX_CPRI_PAYLOAD_SIZE);
        return NULL;
    }

    pthread_mutex_lock(&g_cpri_pool_mutex);
    for (int i = 0; i < MAX_CPRI_MESSAGE_POOL; i++) {
        if (!g_cpri_payload_pool[i].in_use) {
            g_cpri_payload_pool[i].in_use = 1;
            pthread_mutex_unlock(&g_cpri_pool_mutex);
            return g_cpri_payload_pool[i].data;
        }
    }
    pthread_mutex_unlock(&g_cpri_pool_mutex);

    LOG_ERROR("CPRI payload pool exhausted (max=%d)", MAX_CPRI_MESSAGE_POOL);
    return NULL;
}

/* 释放payload缓冲区回池 */
static void cpri_payload_free(uint8_t *ptr)
{
    if (!ptr) {
        return;
    }

    pthread_mutex_lock(&g_cpri_pool_mutex);
    for (int i = 0; i < MAX_CPRI_MESSAGE_POOL; i++) {
        if (g_cpri_payload_pool[i].data == ptr) {
            g_cpri_payload_pool[i].in_use = 0;
            pthread_mutex_unlock(&g_cpri_pool_mutex);
            return;
        }
    }
    pthread_mutex_unlock(&g_cpri_pool_mutex);

    /* 不在内存池中，说明是malloc分配的，使用free释放 */
    //LOG_WARN("Attempted to free unknown payload pointer");

    free(ptr);
}

/* 消息类型名称映射表 */
static const struct {
    uint32_t msg_id;
    const char *name;
} msg_type_names[] = {
    {MSG_CHANNEL_SETUP_REQ, "通道建立请求"},
    {MSG_CHANNEL_SETUP_CFG, "通道建立配置"},
    {MSG_CHANNEL_SETUP_CFG_ACK, "通道建立配置应答"},
    {MSG_VERSION_UPDATE_RESULT_IND, "版本更新结果指示"},
    {MSG_VERSION_UPDATE_RESULT_IND_ACK, "版本更新结果指示应答"},
    {MSG_PAAU_VERSION_QUERY, "PAAU版本查询"},
    {MSG_PAAU_VERSION_QUERY_ACK, "PAAU版本查询应答"},
    {MSG_VERSION_DOWNLOAD_REQ, "版本下载请求"},
    {MSG_VERSION_DOWNLOAD_ACK, "版本下载应答"},
    {MSG_VERSION_DOWNLOAD_RESULT_IND, "版本下载结果指示"},
    {MSG_PAAU_VERSION_ACTIVATE_IND, "PAAU版本激活指示"},
    {MSG_PAAU_VERSION_ACTIVATE_ACK, "PAAU版本激活应答"},
    {MSG_PAAU_STATUS_QUERY, "PAAU状态查询"},
    {MSG_PAAU_STATUS_QUERY_RSP, "PAAU状态查询响应"},
    {MSG_PAAU_PARAM_QUERY, "PAAU参数查询"},
    {MSG_PAAU_PARAM_QUERY_RSP, "PAAU参数查询响应"},
    {MSG_PAAU_PARAM_CONFIG, "PAAU参数配置"},
    {MSG_PAAU_PARAM_CONFIG_RSP, "PAAU参数配置响应"},
    {MSG_PHASED_ARRAY_CALIB_IND, "相控阵校准指示"},
    {MSG_PHASED_ARRAY_CALIB_IND_ACK, "相控阵校准指示应答"},
    {MSG_LOOPBACK_REQ, "环回请求"},
    {MSG_LOOPBACK_REQ_RSP, "环回请求响应"},
    {MSG_ALARM_REPORT_REQ, "告警上报请求"},
    {MSG_ALARM_QUERY_REQ, "告警查询请求"},
    {MSG_ALARM_REPORT_REQ_ACK, "告警上报请求应答"},
    {MSG_LOG_UPLOAD_REQ, "日志上传请求"},
    {MSG_LOG_UPLOAD_REQ_ACK, "日志上传请求应答"},
    {MSG_LOG_UPLOAD_RESULT_IND, "日志上传结果指示"},
    {MSG_RESET_IND, "复位指示"},
    {MSG_REMOTE_PAAU_RESET_IND, "远程PAAU复位指示"},
    {MSG_PAAU_HEARTBEAT, "PAAU在位心跳信息"},
    {MSG_BBU_HEARTBEAT, "BBU在位心跳信息"},
    {MSG_NR_CELL_CONFIG, "NR小区配置消息"},
    {MSG_NR_CELL_CONFIG_RSP, "NR小区配置响应"},
    {0, NULL}
};

const char* cpri_get_msg_type_name(uint32_t msg_id)
{
    for (int i = 0; msg_type_names[i].name != NULL; i++) {
        if (msg_type_names[i].msg_id == msg_id) {
            return msg_type_names[i].name;
        }
    }
    return "未知消息";
}

int cpri_encode_message(const cpri_message_t *msg, uint8_t *buffer, uint32_t buffer_size)
{
    if (!msg || !buffer) {
        return ERROR_INVALID_PARAM;
    }

    uint32_t total_len = CPRI_HEADER_LEN + msg->payload_len;
    if (buffer_size < total_len) {
        LOG_ERROR("Buffer too small: need %u, have %u", total_len, buffer_size);
        return ERROR_INVALID_PARAM;
    }

    /* 编码消息头（小端序） */
    uint32_t offset = 0;
    memcpy(buffer + offset, &msg->header.msg_id, 4);
    offset += 4;

    uint32_t msg_length = total_len;
    memcpy(buffer + offset, &msg_length, 4);
    offset += 4;

    buffer[offset++] = msg->header.paau_id;
    buffer[offset++] = msg->header.bbu_id;
    buffer[offset++] = msg->header.port_num;

    memcpy(buffer + offset, &msg->header.serial_num, 4);
    offset += 4;

    /* 编码载荷 */
    if (msg->payload_len > 0 && msg->payload) {
        memcpy(buffer + offset, msg->payload, msg->payload_len);
    }

    // LOG_DEBUG("Encoded message: type=%s, len=%u",
    //           cpri_get_msg_type_name(msg->header.msg_id), total_len);

    return total_len;
}

int cpri_decode_message(const uint8_t *buffer, uint32_t buffer_len, cpri_message_t *msg)
{
    if (!buffer || !msg || buffer_len < CPRI_HEADER_LEN) {
        return ERROR_INVALID_PARAM;
    }

    memset(msg, 0, sizeof(cpri_message_t));

    /* 解码消息头（小端序） */
    uint32_t offset = 0;
    memcpy(&msg->header.msg_id, buffer + offset, 4);
    offset += 4;

    memcpy(&msg->header.msg_length, buffer + offset, 4);
    offset += 4;

    msg->header.paau_id = buffer[offset++];
    msg->header.bbu_id = buffer[offset++];
    msg->header.port_num = buffer[offset++];

    memcpy(&msg->header.serial_num, buffer + offset, 4);
    offset += 4;

    /* 校验消息长度 */
    if (msg->header.msg_length != buffer_len) {
        LOG_WARN("Message length mismatch: header=%u, actual=%u",
                 msg->header.msg_length, buffer_len);
    }

    /* 解码载荷 */
    msg->payload_len = buffer_len - CPRI_HEADER_LEN;
    if (msg->payload_len > 0) {
        /* 使用静态内存池分配，避免运行时malloc */
        msg->payload = cpri_payload_alloc(msg->payload_len);
        if (!msg->payload) {
            LOG_ERROR("Failed to allocate payload from pool (size=%u)", msg->payload_len);
            return ERROR_MEMORY;
        }
        memcpy(msg->payload, buffer + CPRI_HEADER_LEN, msg->payload_len);
    }

    // LOG_DEBUG("Decoded message: type=%s, len=%u, serial=%u",
    //           cpri_get_msg_type_name(msg->header.msg_id),
    //           msg->header.msg_length, msg->header.serial_num);

    return SUCCESS;
}

void cpri_free_message(cpri_message_t *msg)
{
    if (msg && msg->payload) {
        /* 释放回内存池而不是free */
        //LOG_DEBUG("Freeing message payload: msg_id=%u, payload=%p, len=%u",
         // msg->header.msg_id, (void*)msg->payload, msg->payload_len);

        cpri_payload_free(msg->payload);
        msg->payload = NULL;
        msg->payload_len = 0;
    }
}
