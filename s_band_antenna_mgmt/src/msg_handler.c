#include "msg_handler.h"
#include "channel_setup.h"
#include "heartbeat.h"
#include "status_query.h"
#include "cell_config.h"
#include "param_query.h"
#include "param_config.h"
#include "log_upload.h"
#include "version_manager.h"
#include "alarm_query.h"
#include "loopback.h"
#include "reset.h"
#include "transparent_msg.h"
#include "phased_array_calib.h"
#include "tcp_client.h"
#include "config.h"
#include "logger.h"

/* 外部TCP客户端引用 */
extern tcp_client_t g_tcp_client;

/* ==================== 入站报文身份校验（可配置） ====================
 * 背景：改造前全项目没有任何一处校验入站报文的身份字段，北向链路上任何能冒充
 * BBU 端点（BBU_IP:BBU_PORT）的主机都能下发复位、上注、透传写 FPGA 等指令。
 *
 * 但校验必须与 BBU 侧实际下发的取值对齐，否则一加就直接断链。因此做成三档配置，
 * **默认是"仅记录、不拒绝"**：先跑一段时间看日志里 BBU 到底发的什么值，确认无误
 * 再切到"拒绝"。
 *
 * 配置项（config/antenna_mgmt.conf）：
 *   PAAU_ID           本地 PAAU 标识              默认 0
 *   PAAU_ID_CHECK     0=关闭 1=仅记录 2=拒绝      默认 1
 *   PAAU_ID_BROADCAST 广播值，等于它一律受理      默认 255
 * ================================================================ */
typedef enum {
    PAAU_ID_CHECK_OFF     = 0,  /* 不校验，与改造前行为一致 */
    PAAU_ID_CHECK_WARN    = 1,  /* 不匹配只打 WARN，仍受理 */
    PAAU_ID_CHECK_ENFORCE = 2   /* 不匹配直接丢弃 */
} paau_id_check_mode_t;

static int      g_paau_id_local       = 0;
static int      g_paau_id_broadcast   = 255;
static int      g_paau_id_check_mode  = PAAU_ID_CHECK_WARN;
static bool     g_paau_id_cfg_loaded  = false;
static uint32_t g_paau_id_mismatch_count = 0;

static void paau_id_cfg_load(void)
{
    if (g_paau_id_cfg_loaded) {
        return;
    }

    g_paau_id_local      = config_get_int("PAAU_ID", 0);
    g_paau_id_broadcast  = config_get_int("PAAU_ID_BROADCAST", 255);
    g_paau_id_check_mode = config_get_int("PAAU_ID_CHECK", PAAU_ID_CHECK_WARN);

    if (g_paau_id_check_mode < PAAU_ID_CHECK_OFF ||
        g_paau_id_check_mode > PAAU_ID_CHECK_ENFORCE) {
        LOG_WARN("Invalid PAAU_ID_CHECK=%d, falling back to 1 (warn only)",
                 g_paau_id_check_mode);
        g_paau_id_check_mode = PAAU_ID_CHECK_WARN;
    }

    g_paau_id_cfg_loaded = true;
    LOG_INFO("Inbound paau_id check: mode=%d (0=off 1=warn 2=reject), local=%d, broadcast=%d",
             g_paau_id_check_mode, g_paau_id_local, g_paau_id_broadcast);
}

/* 返回 true 表示应当受理该报文 */
static bool paau_id_accept(uint8_t inbound_id)
{
    paau_id_cfg_load();

    if (g_paau_id_check_mode == PAAU_ID_CHECK_OFF) {
        return true;
    }
    if (g_paau_id_broadcast >= 0 && g_paau_id_broadcast <= 255 &&
        inbound_id == (uint8_t)g_paau_id_broadcast) {
        return true;
    }
    if (inbound_id == (uint8_t)g_paau_id_local) {
        return true;
    }

    g_paau_id_mismatch_count++;
    /* 限流：前 20 次逐条告警，之后每 1000 次一条，避免日志被刷爆 */
    if (g_paau_id_mismatch_count <= 20 || (g_paau_id_mismatch_count % 1000) == 0) {
        LOG_WARN("Inbound paau_id=%u does not match local=%d (broadcast=%d), mismatches=%u",
                 inbound_id, g_paau_id_local, g_paau_id_broadcast,
                 g_paau_id_mismatch_count);
    }

    return (g_paau_id_check_mode != PAAU_ID_CHECK_ENFORCE);
}

/* 消息处理函数指针类型 */
typedef int (*msg_handler_func_t)(const cpri_message_t *msg);

/* 消息处理映射表 */
typedef struct {
    uint32_t msg_id;
    msg_handler_func_t handler;
} msg_handler_map_t;

/* 消息处理映射表 */
static msg_handler_map_t g_msg_handlers[] = {
    {MSG_CHANNEL_SETUP_CFG, handle_channel_setup_req},
    {MSG_BBU_HEARTBEAT, handle_heartbeat},
    {MSG_PHASED_ARRAY_CALIB_IND, handle_phased_array_calib_req},
    {MSG_PAAU_STATUS_QUERY, handle_status_query},
    {MSG_PAAU_PARAM_QUERY, handle_param_query},
    {MSG_PAAU_PARAM_CONFIG, handle_param_config},
    {MSG_ALARM_QUERY_REQ, handle_alarm_query_req},
    {MSG_NR_CELL_CONFIG, handle_cell_config},
    {MSG_LOG_UPLOAD_REQ, handle_log_upload_req},
    {MSG_VERSION_DOWNLOAD_REQ, handle_version_download_req},
    {MSG_PAAU_VERSION_ACTIVATE_IND, handle_version_activate_ind},
    {MSG_LOOPBACK_REQ, handle_loopback_req},
    {MSG_RESET_IND, handle_reset_ind},
    {MSG_REMOTE_RESET_IND, handle_remote_reset_ind},
    {0, NULL}
};

int msg_handler_init(void)
{
    LOG_INFO("Message handler initialized");
    return SUCCESS;
}

/* ==================== 入站 msg_id 白名单 ====================
 * 只接受分发表里已登记的消息类型，以及透传区间 221-240。
 *
 * 顺带修掉一处截断：msg_id 在报文头里是 uint32_t，而透传判定的形参原先是 uint16_t，
 * 于是 msg_id = 0x10000 + 221 这类构造会被截断成 221 而被【误路由进透传分支】。
 * 实测（基线二进制）：msg_id=65757 会打出 "Handle transparent message"。
 *
 * 定级说明：该误路由【不会】直达 FPGA 写操作——透传处理内部的方向判定用的是完整
 * 32 位值，65757 会落到 "unexpected direction" 分支后返回。任何能通过方向判定的值
 * 本身就已落在 [231,240] 内，截断与否结果相同。所以这是路由缺陷，不是写权限绕过；
 * 但仍应由白名单在入口处挡掉，且透传判据理应与报文头类型保持一致。
 * ========================================================== */
static bool msg_id_is_accepted(uint32_t msg_id)
{
    if (is_transparent_message(msg_id)) {
        return true;
    }
    for (int i = 0; g_msg_handlers[i].handler != NULL; i++) {
        if (g_msg_handlers[i].msg_id == msg_id) {
            return true;
        }
    }
    return false;
}

int msg_handler_dispatch(const cpri_message_t *msg)
{
    if (!msg) {
        return ERROR_INVALID_PARAM;
    }

    /* 入站 msg_id 白名单 */
    if (!msg_id_is_accepted(msg->header.msg_id)) {
        LOG_WARN("Rejected unknown msg_id=%u (serial=%u)",
                 msg->header.msg_id, msg->header.serial_num);
        return ERROR_INVALID_PARAM;
    }

    /* 入站身份校验（可配置，默认仅记录不拒绝）。放在分发之前，
     * 使透传消息（221-240，会裸写 FPGA）也一并纳入校验。 */
    if (!paau_id_accept(msg->header.paau_id)) {
        LOG_ERROR("Message DROPPED: paau_id=%u not accepted (msg=%s, serial=%u)",
                  msg->header.paau_id,
                  cpri_get_msg_type_name(msg->header.msg_id),
                  msg->header.serial_num);
        return ERROR_INVALID_PARAM;
    }

    LOG_INFO("Dispatching message: type=%s, serial=%u",
             cpri_get_msg_type_name(msg->header.msg_id),
             msg->header.serial_num);

    /* 检查是否为透传消息 (221-240) */
    if (is_transparent_message(msg->header.msg_id)) {
        return handle_transparent_msg(msg);
    }

    /* 查找对应的处理函数 */
    for (int i = 0; g_msg_handlers[i].handler != NULL; i++) {
        if (g_msg_handlers[i].msg_id == msg->header.msg_id) {
            return g_msg_handlers[i].handler(msg);
        }
    }

    LOG_WARN("No handler for message type: %s",
             cpri_get_msg_type_name(msg->header.msg_id));
    return ERROR_GENERAL;
}

void msg_handler_destroy(void)
{
    LOG_INFO("Message handler destroyed");
}

/* 默认消息处理函数实现 */
int handle_channel_setup_req(const cpri_message_t *msg)
{
    LOG_INFO("Handle channel setup config");

    /* 处理通道建立配置 */
    int ret = channel_setup_handle_config(msg);
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to handle channel setup config");
        return ret;
    }

    /* 发送通道建立响应，带回相同的流水号 */
    cpri_message_t response;
    ret = channel_setup_create_response(&response, 0);  /* 0 = 成功 */
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to create channel setup response");
        return ret;
    }

    /* 复制请求消息的流水号到响应消息 */
    response.header.serial_num = msg->header.serial_num;
    response.header.bbu_id = msg->header.bbu_id;
    response.header.port_num = msg->header.port_num;

    /* 编码并发送响应 */
    uint8_t buffer[2048];
    int len = cpri_encode_message(&response, buffer, sizeof(buffer));
    if (len > 0) {
        tcp_client_send(&g_tcp_client, buffer, len);
        LOG_INFO("Sent channel setup response (serial_num=%u)", response.header.serial_num);
    }

    cpri_free_message(&response);

    /* 通道建立完成，启动心跳 */
    heartbeat_start();
    LOG_INFO("Channel established, heartbeat started");

    return SUCCESS;
}

int handle_heartbeat(const cpri_message_t *msg)
{
    //LOG_DEBUG("Handle BBU heartbeat");
    return heartbeat_handle_bbu(msg);
}

int handle_status_query(const cpri_message_t *msg)
{
    LOG_INFO("Handle status query");

    /* 处理状态查询请求 */
    int ret = status_query_handle_request(msg);
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to handle status query request");
        return ret;
    }

    /* 生成状态查询响应 */
    cpri_message_t response;
    ret = status_query_create_response(&response, msg);
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to create status query response");
        return ret;
    }

    /* 编码并发送响应 */
    uint8_t buffer[2048];
    int len = cpri_encode_message(&response, buffer, sizeof(buffer));
    if (len > 0) {
        tcp_client_send(&g_tcp_client, buffer, len);
        LOG_INFO("Sent status query response (serial_num=%u)", response.header.serial_num);
    }

    cpri_free_message(&response);
    return SUCCESS;
}

int handle_param_query(const cpri_message_t *msg)
{
    LOG_INFO("Handle param query (serial=%u)", msg->header.serial_num);

    /* 处理参数查询请求 */
    cpri_message_t response;
    int ret = param_query_handle_request(msg, &response);
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to handle param query request");
        return ret;
    }

    /* 编码并发送响应 */
    uint8_t buffer[2048];
    int len = cpri_encode_message(&response, buffer, sizeof(buffer));
    if (len > 0) {
        tcp_client_send(&g_tcp_client, buffer, len);
        LOG_INFO("Sent param query response (serial=%u, %d bytes)",
                 response.header.serial_num, len);
    } else {
        LOG_ERROR("Failed to encode param query response");
    }

    cpri_free_message(&response);
    return (len > 0) ? SUCCESS : ERROR_GENERAL;
}

int handle_param_config(const cpri_message_t *msg)
{
    LOG_INFO("Handle param config");

    cpri_message_t response;
    memset(&response, 0, sizeof(response));

    /* 处理参数配置请求 */
    int ret = param_config_handle_request(msg, &response);
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to handle param config request: %d", ret);
        return ret;
    }

    /* 编码并发送响应 */
    uint8_t buffer[2048];
    int len = cpri_encode_message(&response, buffer, sizeof(buffer));
    if (len <= 0) {
        LOG_ERROR("Failed to encode param config response");
        /* 注意：response.payload指向静态缓冲区，不需要释放 */
        return ERROR_GENERAL;
    }

    ret = tcp_client_send(&g_tcp_client, buffer, len);
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to send param config response");
    }

    /* 注意：response.payload指向静态缓冲区，不需要释放 */
    return (len > 0) ? SUCCESS : ERROR_GENERAL;
}

int handle_cell_config(const cpri_message_t *msg)
{
    LOG_INFO("Handle NR cell config");

    /* 交给 cell_config 的 worker 线程处理：
     * 频点配置需要等待最长 5 秒的天线模式校验，不能阻塞 TCP 接收线程
     * （阻塞期间不读 socket 会把 BBU 心跳堵在内核缓冲，误判 BBU 失联）。
     * 响应由 worker 在校验结束后发出，对 BBU 而言内容与时机不变。 */
    int ret = cell_config_submit_request(msg);
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to submit cell config request");
        return ret;
    }

    return SUCCESS;
}

int handle_log_upload_req(const cpri_message_t *msg)
{
    LOG_INFO("Handle log upload request");
    return log_upload_handle_request(msg);
}

int handle_version_download_req(const cpri_message_t *msg)
{
    LOG_INFO("Handle version download request");
    return version_handle_download_request(msg);
}

int handle_version_activate_ind(const cpri_message_t *msg)
{
    LOG_INFO("Handle version activate indication");
    return version_handle_activate_request(msg);
}

int handle_alarm_query_req(const cpri_message_t *msg)
{
    LOG_INFO("Handle alarm query request");

    if (!msg || !msg->payload) {
        LOG_ERROR("Invalid alarm query message");
        return ERROR_INVALID_PARAM;
    }

    /* 跳过 IE 头部（4字节：2字节type + 2字节length），解析IE数据 */
    if (msg->payload_len < 4 + sizeof(ie_alarm_query_req_t)) {
        LOG_ERROR("Alarm query payload too short: %u bytes", msg->payload_len);
        return ERROR_INVALID_PARAM;
    }

    const ie_alarm_query_req_t *req_ie = (const ie_alarm_query_req_t *)(msg->payload + 4);

    LOG_DEBUG("Parsed alarm query: code=0x%08X, subcode=0x%08X",
              req_ie->alarm_code, req_ie->sub_code);

    return handle_alarm_query_request(&msg->header, req_ie);
}

int handle_loopback_req(const cpri_message_t *msg)
{
    LOG_INFO("Handle loopback request");

    if (!msg || !msg->payload) {
        LOG_ERROR("Invalid loopback message");
        return ERROR_INVALID_PARAM;
    }

    /* 跳过 IE 头部（4字节：2字节type + 2字节length），解析IE数据 */
    if (msg->payload_len < 4 + sizeof(ie_loopback_req_t)) {
        LOG_ERROR("Loopback payload too short: %u bytes", msg->payload_len);
        return ERROR_INVALID_PARAM;
    }

    const ie_loopback_req_t *req_ie = (const ie_loopback_req_t *)(msg->payload + 4);

    /* 获取来源IP */
    const char *source_ip = config_get_string("BBU_IP", "unknown");

    return handle_loopback_request(&msg->header, req_ie, source_ip);
}

int handle_reset_ind(const cpri_message_t *msg)
{
    LOG_INFO("Handle reset indication");

    if (!msg || !msg->payload) {
        LOG_ERROR("Invalid reset message");
        return ERROR_INVALID_PARAM;
    }

    /* 跳过 IE 头部（4字节：2字节type + 2字节length），解析IE数据 */
    if (msg->payload_len < 4 + sizeof(ie_reset_ind_t)) {
        LOG_ERROR("Reset payload too short: %u bytes", msg->payload_len);
        return ERROR_INVALID_PARAM;
    }

    const ie_reset_ind_t *ind_ie = (const ie_reset_ind_t *)(msg->payload + 4);

    /* 获取来源IP */
    const char *source_ip = config_get_string("BBU_IP", "unknown");

    return handle_reset_indication(ind_ie, source_ip, MSG_RESET_IND);
}

int handle_remote_reset_ind(const cpri_message_t *msg)
{
    LOG_INFO("Handle remote reset indication");

    if (!msg || !msg->payload) {
        LOG_ERROR("Invalid remote reset message");
        return ERROR_INVALID_PARAM;
    }

    /* 跳过 IE 头部（4字节：2字节type + 2字节length），解析IE数据 */
    if (msg->payload_len < 4 + sizeof(ie_reset_ind_t)) {
        LOG_ERROR("Remote reset payload too short: %u bytes", msg->payload_len);
        return ERROR_INVALID_PARAM;
    }

    const ie_reset_ind_t *ind_ie = (const ie_reset_ind_t *)(msg->payload + 4);

    /* 获取来源IP */
    const char *source_ip = config_get_string("BBU_IP", "unknown");

    return handle_reset_indication(ind_ie, source_ip, MSG_REMOTE_PAAU_RESET_IND);
}

int handle_transparent_msg(const cpri_message_t *msg)
{
    LOG_INFO("Handle transparent message");

    if (!msg || !msg->payload) {
        LOG_ERROR("Invalid transparent message");
        return ERROR_INVALID_PARAM;
    }

    /* 获取来源IP */
    const char *source_ip = config_get_string("BBU_IP", "unknown");

    return transparent_msg_handle(msg, source_ip);
}

int handle_phased_array_calib_req(const cpri_message_t *msg)
{
    LOG_INFO("Handle phased array calibration request");

    if (!msg || !msg->payload) {
        LOG_ERROR("Invalid phased array calibration message");
        return ERROR_INVALID_PARAM;
    }

    /* 跳过 IE 头部（4字节：2字节type + 2字节length），解析IE数据 */
    /* 根据协议，IE 751 可能没有数据部分，只有 IE 头 */
    const ie_phased_array_calib_ind_t *req_ie = NULL;
    if (msg->payload_len > 4) {
        req_ie = (const ie_phased_array_calib_ind_t *)(msg->payload + 4);
    }

    return handle_phased_array_calib_request(&msg->header, req_ie);
}
