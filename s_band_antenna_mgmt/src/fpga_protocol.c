#include <arpa/inet.h>
#include <pthread.h>
#include "fpga_protocol.h"
#include "logger.h"

/* 静态内存池配置 - FPGA消息通常较小 */
#define MAX_FPGA_PAYLOAD_SIZE 2048
#define MAX_FPGA_MESSAGE_POOL 8

/* 静态内存池 */
typedef struct {
    uint8_t in_use;
    uint8_t data[MAX_FPGA_PAYLOAD_SIZE];
} fpga_payload_buffer_t;

static fpga_payload_buffer_t g_fpga_payload_pool[MAX_FPGA_MESSAGE_POOL];
static pthread_mutex_t g_fpga_pool_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 从池中分配payload缓冲区 */
static uint8_t* fpga_payload_alloc(uint32_t size)
{
    if (size > MAX_FPGA_PAYLOAD_SIZE) {
        LOG_ERROR("FPGA payload size %u exceeds max %u", size, MAX_FPGA_PAYLOAD_SIZE);
        return NULL;
    }

    pthread_mutex_lock(&g_fpga_pool_mutex);
    for (int i = 0; i < MAX_FPGA_MESSAGE_POOL; i++) {
        if (!g_fpga_payload_pool[i].in_use) {
            g_fpga_payload_pool[i].in_use = 1;
            pthread_mutex_unlock(&g_fpga_pool_mutex);
            return g_fpga_payload_pool[i].data;
        }
    }
    pthread_mutex_unlock(&g_fpga_pool_mutex);

    LOG_ERROR("FPGA payload pool exhausted (max=%d)", MAX_FPGA_MESSAGE_POOL);
    return NULL;
}

/* 释放payload缓冲区回池 */
static void fpga_payload_free_internal(uint8_t *ptr)
{
    if (!ptr) {
        return;
    }

    pthread_mutex_lock(&g_fpga_pool_mutex);
    for (int i = 0; i < MAX_FPGA_MESSAGE_POOL; i++) {
        if (g_fpga_payload_pool[i].data == ptr) {
            g_fpga_payload_pool[i].in_use = 0;
            pthread_mutex_unlock(&g_fpga_pool_mutex);
            return;
        }
    }
    pthread_mutex_unlock(&g_fpga_pool_mutex);

    LOG_WARN("Attempted to free unknown FPGA payload pointer");
}

/* 频率编码：将频率(kHz)转换为压缩格式 */
uint32_t fpga_encode_frequency(uint32_t freq_khz)
{
    uint8_t quotient = freq_khz / 50000;
    uint32_t remainder = (freq_khz % 50000) * 2;
    uint32_t encoded = (quotient << 24) | (remainder & 0xFFFFFF);

    LOG_DEBUG("Encode frequency: %u kHz -> 0x%08X (quotient=%u, remainder=%u)",
              freq_khz, encoded, quotient, remainder);
    return encoded;
}

/* 频率解码：将压缩格式转换为频率(kHz) */
uint32_t fpga_decode_frequency(uint32_t encoded_freq)
{
    uint8_t quotient = (encoded_freq >> 24) & 0xFF;
    uint32_t remainder = encoded_freq & 0xFFFFFF;
    uint32_t freq_khz = quotient * 50000 + remainder / 2;

    // LOG_DEBUG("Decode frequency: 0x%08X -> %u kHz (quotient=%u, remainder=%u)",
    //           encoded_freq, freq_khz, quotient, remainder);
    return freq_khz;
}

int fpga_encode_message(const fpga_message_t *msg, uint8_t *buffer, uint32_t buffer_size)
{
    if (!msg || !buffer) {
        return ERROR_INVALID_PARAM;
    }

    /* 计算总长度：帧头(2) + 消息ID(1) + 长度(1) + 数据(N) + 帧尾(2) */
    uint32_t total_len = FPGA_HEADER_SIZE + FPGA_MSG_ID_SIZE + FPGA_LENGTH_SIZE + msg->payload_len + FPGA_TAIL_SIZE;
    if (buffer_size < total_len) {
        LOG_ERROR("Buffer too small for FPGA message: need %u, have %u", total_len, buffer_size);
        return ERROR_INVALID_PARAM;
    }

    /* 长度字段：消息ID(1) + 长度字段本身(1) + 数据(N) */
    uint8_t length_field =  msg->payload_len;

    uint32_t offset = 0;

    /* 帧头（大端） */
    uint16_t header = HTONS(FPGA_FRAME_HEADER);
    memcpy(buffer + offset, &header, 2);
    offset += 2;

    /* 消息ID */
    buffer[offset++] = msg->msg_id;

    /* 长度字段 */
    buffer[offset++] = length_field;

    /* 载荷数据 */
    if (msg->payload_len > 0 && msg->payload) {
        memcpy(buffer + offset, msg->payload, msg->payload_len);
        offset += msg->payload_len;
    }

    /* 帧尾（大端） */
    uint16_t tail = HTONS(FPGA_FRAME_TAIL);
    memcpy(buffer + offset, &tail, 2);
    offset += 2;

    // LOG_DEBUG("Encoded FPGA message: ID=0x%02X, length=%u, total=%u", msg->msg_id, length_field, total_len);
    return total_len;
}

int fpga_decode_message(const uint8_t *buffer, uint32_t buffer_len, fpga_message_t *msg)
{
    if (!buffer || !msg || buffer_len < FPGA_MIN_FRAME_SIZE) {
        return ERROR_INVALID_PARAM;
    }

    /* 检查帧头 */
    uint16_t header;
    memcpy(&header, buffer, 2);
    header = NTOHS(header);
    if (header != FPGA_FRAME_HEADER) {
        LOG_ERROR("Invalid FPGA frame header: 0x%04X", header);
        return ERROR_GENERAL;
    }

    /* 检查帧尾 */
    uint16_t tail;
    memcpy(&tail, buffer + buffer_len - 2, 2);
    tail = NTOHS(tail);
    if (tail != FPGA_FRAME_TAIL) {
        LOG_ERROR("Invalid FPGA frame tail: 0x%04X", tail);
        return ERROR_GENERAL;
    }

    /* 解析消息ID */
    msg->msg_id = buffer[2];

    /* 解析长度字段 */
    uint8_t length_field = buffer[3];

    /* 校验长度 */
    uint32_t expected_total_len = FPGA_HEADER_SIZE + length_field + FPGA_TAIL_SIZE;
    if (buffer_len != expected_total_len) {
        LOG_WARN("FPGA frame length mismatch: expected=%u, actual=%u", expected_total_len, buffer_len);
    }

    /* 解析载荷：长度字段包含了消息ID(1) + 长度字段本身(1) + 数据(N) */
    /* 所以数据长度 = length_field - 2 */
    msg->payload_len = (length_field >= 2) ? (length_field - 2) : 0;

    if (msg->payload_len > 0) {
        /* 使用静态内存池分配，避免运行时malloc */
        msg->payload = fpga_payload_alloc(msg->payload_len);
        if (!msg->payload) {
            LOG_ERROR("Failed to allocate FPGA payload from pool (size=%u)", msg->payload_len);
            return ERROR_MEMORY;
        }
        memcpy(msg->payload, buffer + 4, msg->payload_len);
    } else {
        msg->payload = NULL;
    }

    // LOG_DEBUG("Decoded FPGA message: ID=0x%02X, length=%u, payload_len=%u",
    //           msg->msg_id, length_field, msg->payload_len);
    return SUCCESS;
}

int fpga_decode_status_frame(const uint8_t *buffer, uint32_t buffer_len, fpga_status_frame_t *status)
{
    if (!buffer || !status || buffer_len != sizeof(fpga_status_frame_t)) {
        LOG_ERROR("Invalid parameters for status frame decode: buffer_len=%u, expected=%zu",
                  buffer_len, sizeof(fpga_status_frame_t));
        return ERROR_INVALID_PARAM;
    }

    /* 分段解析，避免结构体对齐问题 */
    uint32_t offset = 0;

    /* 解析帧头（2字节，大端） */
    memcpy(&status->frame_header, buffer + offset, 2);
    status->frame_header = NTOHS(status->frame_header);
    offset += 2;

    /* 解析长度字段（2字节，大端） */
    uint16_t length_raw;
    memcpy(&length_raw, buffer + offset, 2);
    status->length = NTOHS(length_raw);
    offset += 2;

    /* 解析数据部分（127字节） */
    memcpy(&status->data, buffer + offset, sizeof(fpga_status_data_t));
    offset += sizeof(fpga_status_data_t);

    /* 解析帧尾（2字节，大端） */
    memcpy(&status->frame_tail, buffer + offset, 2);
    status->frame_tail = NTOHS(status->frame_tail);

    /* 验证帧头帧尾 */
    if (status->frame_header != FPGA_FRAME_HEADER || status->frame_tail != FPGA_FRAME_TAIL) {
        LOG_ERROR("Invalid status frame header/tail: 0x%04X/0x%04X",
                  status->frame_header, status->frame_tail);
        return ERROR_GENERAL;
    }

    /* 验证长度字段：应该是 127 */
    if (status->length != 127) {
        LOG_WARN("Unexpected status frame length field: %u (expected 127)", status->length);
    }

    /* 转换数据部分的字节序 */
    status->data.nr_beam_count = NTOHL(status->data.nr_beam_count);
    status->data.max_tx_power = NTOHS(status->data.max_tx_power);
    status->data.supported_modes = NTOHS(status->data.supported_modes);
    status->data.hw_version = NTOHL(status->data.hw_version);
    status->data.current_output_power = NTOHS(status->data.current_output_power);
    status->data.reserved1 = NTOHS(status->data.reserved1);
    status->data.beam_status = NTOHL(status->data.beam_status);
    status->data.lo_frequency = NTOHL(status->data.lo_frequency);
    status->data.lo_lock_status = NTOHL(status->data.lo_lock_status);
    status->data.clock_sync_status = NTOHL(status->data.clock_sync_status);
    status->data.phased_array_work_mode = NTOHL(status->data.phased_array_work_mode);
    status->data.cpri_work_mode = NTOHL(status->data.cpri_work_mode);
    status->data.toffset = NTOHL(status->data.toffset);
    status->data.t2a = NTOHL(status->data.t2a);
    status->data.ta3 = NTOHL(status->data.ta3);
    status->data.tx_freq_start = NTOHS(status->data.tx_freq_start);
    status->data.tx_freq_end = NTOHS(status->data.tx_freq_end);
    status->data.rx_freq_start = NTOHS(status->data.rx_freq_start);
    status->data.rx_freq_end = NTOHS(status->data.rx_freq_end);
    status->data.beam_bandwidth = NTOHS(status->data.beam_bandwidth);
    status->data.reserved5 = NTOHL(status->data.reserved5);

    // LOG_DEBUG("Decoded FPGA status frame successfully");
    // LOG_DEBUG("  Link: main_fiber=%u, backup_fiber=%u, link_flag=0x%02X",
    //           status->data.main_fiber_num, status->data.backup_fiber_num, status->data.link_success_flag);
    // LOG_DEBUG("  Capability: beams=%u, max_power=%u, modes=0x%04X",
    //           status->data.nr_beam_count, status->data.max_tx_power, status->data.supported_modes);
    // LOG_DEBUG("  LO: freq_encoded=0x%08X, lock=%u", status->data.lo_frequency, status->data.lo_lock_status);
    // LOG_DEBUG("  Clock: sync=%u", status->data.clock_sync_status);
    // LOG_DEBUG("  Temp: T1=%d°C, T2=%d°C, T3=%d°C", status->data.temp1, status->data.temp2, status->data.temp3);
    // LOG_DEBUG("  phased_array_work_mode: =%d, cpri_work_mode=%d", status->data.phased_array_work_mode,  status->data.cpri_work_mode);

    return SUCCESS;
}

void fpga_free_message(fpga_message_t *msg)
{
    if (msg && msg->payload) {
        /* 释放回内存池而不是free */
        fpga_payload_free_internal(msg->payload);
        msg->payload = NULL;
        msg->payload_len = 0;
    }
}
