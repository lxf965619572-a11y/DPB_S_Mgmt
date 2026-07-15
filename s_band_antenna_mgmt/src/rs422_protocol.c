#include "rs422_protocol.h"
#include "logger.h"
#include <string.h>
#include <arpa/inet.h>

/* 帧同步字 */
#define RS422_SYNC_WORD_CMD             0xEB90  /* 指令帧标识符 */
#define RS422_SYNC_WORD_ACK             0x1ACF  /* 应答帧标识符 */

/* 序列计数器(全局,每帧递增) */
static uint16_t g_seq_counter = 0;

int rs422_protocol_init(void)
{
    g_seq_counter = 0;
    LOG_INFO("RS-422 protocol initialized");
    return SUCCESS;
}

void rs422_protocol_cleanup(void)
{
    LOG_INFO("RS-422 protocol cleaned up");
}

/* 回滚序列计数器(用于重发时保持序列号不变) */
void rs422_seq_counter_rollback(void)
{
    if (g_seq_counter > 0) {
        g_seq_counter--;
    }
}

/* 重置序列计数器为0(用于开始新文件传输) */
void rs422_seq_counter_reset(void)
{
    g_seq_counter = 0;
    LOG_DEBUG("序列计数器已重置为0");
}

/* 计算校验和: 单字节累加求和取反,取低16位 (用于帧校验) */
uint16_t rs422_checksum(const uint8_t *data, uint32_t len)
{
    uint32_t sum = 0;

    /* 单字节累加求和 */
    for (uint32_t i = 0; i < len; i++) {
        sum += data[i];
    }

    /* 取反,取低16位 */
    return (uint16_t)(~sum & 0xFFFF);
}

/* 计算CRC16-CCITT-FALSE (用于文件校验) */
uint16_t rs422_crc16_ccitt_false(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFF;  /* CRC16-CCITT-FALSE初始值 */
    const uint16_t poly = 0x1021;

    for (uint32_t i = 0; i < len; i++) {
        crc ^= ((uint16_t)data[i] << 8);
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ poly;
            } else {
                crc = crc << 1;
            }
        }
    }

    return crc;
}

int rs422_encode_frame(uint16_t apid, uint16_t cmd_code,
                       const uint8_t *payload, uint32_t payload_len,
                       uint8_t *frame_buf, uint32_t frame_buf_size)
{
    if (!frame_buf || frame_buf_size < RS422_HEADER_SIZE + payload_len + RS422_CRC_SIZE + 2) {
        return ERROR_INVALID_PARAM;
    }

    if (payload_len > RS422_MAX_PAYLOAD_SIZE - 2) {  /* -2用于cmd_code */
        return ERROR_INVALID_PARAM;
    }

    uint32_t offset = 0;

    /* 1. 标识符 (16bit) */
    uint16_t sync = htons(RS422_SYNC_WORD_CMD);
    memcpy(frame_buf + offset, &sync, 2);
    offset += 2;

    /* 2. 包标识 (16bit): 版本号(3bit) + 类型(1bit) + 副导头标识(1bit) + APID(11bit) */
    /* 版本号=0b000, 类型=0b0, 副导头标识=0b0, APID=11bit */
    uint16_t packet_id = (apid & 0x07FF);  /* 只取APID的低11位 */
    uint16_t packet_id_be = htons(packet_id);
    memcpy(frame_buf + offset, &packet_id_be, 2);
    offset += 2;

    /* 3. 包序列控制 (16bit): 分组标志(2bit) + 源包序列计数(14bit) */
    /* 分组标志=0b11(控制指令,单帧), 序列计数=14bit */
    uint16_t seq_ctrl = (0x3 << 14) | (g_seq_counter & 0x3FFF);  /* 0b11 + 14bit计数 */
    g_seq_counter++;
    uint16_t seq_ctrl_be = htons(seq_ctrl);
    memcpy(frame_buf + offset, &seq_ctrl_be, 2);
    offset += 2;

    /* 4. 数据域长度 (16bit): 包数据长度-1 */
    uint16_t data_len = 2 + payload_len - 1;  /* cmd_code(2) + payload - 1 */
    uint16_t data_len_be = htons(data_len);
    memcpy(frame_buf + offset, &data_len_be, 2);
    offset += 2;

    /* 5. 命令码 (2字节) */
    uint16_t cmd_be = htons(cmd_code);
    memcpy(frame_buf + offset, &cmd_be, 2);
    offset += 2;

    /* 6. 载荷 */
    if (payload && payload_len > 0) {
        memcpy(frame_buf + offset, payload, payload_len);
        offset += payload_len;
    }

    /* 7. 校验和 (对包主导头(不含标识符)和数据域计算) */
    /* 从offset=2开始(跳过标识符),到当前offset结束 */
    uint16_t checksum = rs422_checksum(frame_buf + 2, offset - 2);
    uint16_t checksum_be = htons(checksum);
    memcpy(frame_buf + offset, &checksum_be, 2);
    offset += 2;

    return offset;
}

int rs422_decode_frame(const uint8_t *frame_buf, uint32_t frame_len,
                       uint16_t *apid, uint16_t *cmd_code,
                       const uint8_t **payload, uint32_t *payload_len)
{
    if (!frame_buf || frame_len < RS422_HEADER_SIZE + 2 + RS422_CRC_SIZE) {
        return ERROR_INVALID_PARAM;
    }

    uint32_t offset = 0;

    /* 验证同步字 (支持指令帧和应答帧) */
    uint16_t sync;
    memcpy(&sync, frame_buf + offset, 2);
    sync = ntohs(sync);
    if (sync != RS422_SYNC_WORD_ACK) {
        LOG_ERROR("无效的同步字: 0x%04X (期望: 0x%04X)",
                  sync,  RS422_SYNC_WORD_ACK);
        return ERROR_GENERAL;
    }
    offset += 2;

    /* 提取APID */
    uint16_t apid_val;
    memcpy(&apid_val, frame_buf + offset, 2);
    apid_val = ntohs(apid_val);
    if (apid) *apid = apid_val;
    offset += 2;

    /* 跳过序列计数器 */
    offset += 2;

    /* 提取数据长度 */
    uint16_t data_len;
    memcpy(&data_len, frame_buf + offset, 2);
    data_len = ntohs(data_len);
    offset += 2;

    /* 验证帧长度 (data_len是实际长度-1,所以要+1) */
    uint32_t actual_data_len = data_len + 1;
    if (frame_len < RS422_HEADER_SIZE + actual_data_len + RS422_CRC_SIZE) {
        LOG_ERROR("帧长度不匹配: 实际 %u, 期望 %u",
                  frame_len, RS422_HEADER_SIZE + actual_data_len + RS422_CRC_SIZE);
        return ERROR_GENERAL;
    }

    /* 验证校验和 (对包主导头(不含标识符)和数据域计算) */
    uint32_t checksum_offset = RS422_HEADER_SIZE + actual_data_len;
    /* 从offset=2开始(跳过标识符),到checksum_offset结束 */
    uint16_t expected_checksum = rs422_checksum(frame_buf + 2, checksum_offset - 2);
    uint16_t expected_checksum_be = htons(expected_checksum);
    uint16_t actual_checksum_be;
    memcpy(&actual_checksum_be, frame_buf + checksum_offset, 2);

    if (actual_checksum_be != expected_checksum_be) {
        LOG_ERROR("校验和不匹配: 实际 0x%04X, 期望 0x%04X", ntohs(actual_checksum_be), expected_checksum);
        return ERROR_GENERAL;
    }

    /* 提取命令码 */
    uint16_t cmd;
    //uint8_t cmd_data;
    memcpy(&cmd, frame_buf + offset, 2);
    //memcpy(&cmd_data, frame_buf + offset + 2, 1);
    cmd = ntohs(cmd);
    //LOG_DEBUG("回复的指令码: 0x%04X 0x%04X" , cmd , cmd_data);
    if (cmd_code) *cmd_code = cmd;
    offset += 2;

    /* 提取载荷 (data_len是实际长度-1,所以要+1再减去cmd_code的2字节) */
    uint32_t pl_len = actual_data_len - 2;  /* -2 for cmd_code */
    if (payload) *payload = frame_buf + offset;
    if (payload_len) *payload_len = pl_len;

    return SUCCESS;
}

int rs422_build_reconfig_start(uint8_t file_type, uint8_t file_sub_type,
                                uint8_t *frame_buf, uint32_t frame_buf_size)
{
    rs422_cmd_reconfig_start_t cmd;
    cmd.file_type = file_type;
    cmd.file_sub_type = file_sub_type;

    return rs422_encode_frame(RS422_APID_CONTROL, RS422_CMD_RECONFIG_START,
                              (const uint8_t *)&cmd, sizeof(cmd),
                              frame_buf, frame_buf_size);
}

int rs422_build_reconfig_query(uint8_t *frame_buf, uint32_t frame_buf_size)
{
    return rs422_encode_frame(RS422_APID_CONTROL, RS422_CMD_RECONFIG_QUERY,
                              NULL, 0, frame_buf, frame_buf_size);
}

int rs422_parse_reconfig_ack(const uint8_t *payload, uint32_t payload_len, uint8_t *result)
{
    if (!payload || payload_len < sizeof(rs422_cmd_reconfig_ack_t) || !result) {
        return ERROR_INVALID_PARAM;
    }

    const rs422_cmd_reconfig_ack_t *ack = (const rs422_cmd_reconfig_ack_t *)payload;
    *result = ack->result;
    return SUCCESS;
}

int rs422_build_version_query(uint8_t *frame_buf, uint32_t frame_buf_size)
{
    return rs422_encode_frame(RS422_APID_CONTROL, RS422_CMD_VERSION_QUERY,
                              NULL, 0, frame_buf, frame_buf_size);
}

int rs422_parse_version_ack(const uint8_t *payload, uint32_t payload_len, char *version)
{
    if (!payload || payload_len < sizeof(rs422_cmd_version_ack_t) || !version) {
        return ERROR_INVALID_PARAM;
    }

    const rs422_cmd_version_ack_t *ack = (const rs422_cmd_version_ack_t *)payload;
    memcpy(version, ack->version, 32);
    version[31] = '\0';  /* 确保字符串以null结尾 */
    return SUCCESS;
}

int rs422_build_transfer_start(const rs422_cmd_transfer_start_t *params,
                                uint8_t *frame_buf, uint32_t frame_buf_size)
{
    if (!params) {
        return ERROR_INVALID_PARAM;
    }

    /* 转换为网络字节序 */
    rs422_cmd_transfer_start_t cmd;
    cmd.device_id = params->device_id;
    cmd.file_type = params->file_type;
    cmd.file_sub_type = params->file_sub_type;
    cmd.segment_info = htons(params->segment_info);
    cmd.file_length = htonl(params->file_length);
    cmd.last_segment_len = htonl(params->last_segment_len);
    cmd.file_checksum = htons(params->file_checksum);

    return rs422_encode_frame(RS422_APID_CONTROL, RS422_CMD_TRANSFER_START,
                              (const uint8_t *)&cmd, sizeof(cmd),
                              frame_buf, frame_buf_size);
}

int rs422_parse_transfer_start_ack(const uint8_t *payload, uint32_t payload_len, uint8_t *result)
{
    if (!payload || payload_len < sizeof(rs422_cmd_transfer_start_ack_t) || !result) {
        return ERROR_INVALID_PARAM;
    }

    const rs422_cmd_transfer_start_ack_t *ack = (const rs422_cmd_transfer_start_ack_t *)payload;
    *result = ack->result;
    return SUCCESS;
}

int rs422_build_file_data(uint16_t segment_num, const uint8_t *data, uint32_t data_len,
                           uint8_t *frame_buf, uint32_t frame_buf_size,
                           uint32_t packet_index, uint32_t total_packets_in_segment)
{
    if (!data || data_len == 0 || !frame_buf) {
        return ERROR_INVALID_PARAM;
    }

    /* 检查缓冲区大小 */
    uint32_t required_size = RS422_HEADER_SIZE + 2 + data_len + RS422_CRC_SIZE;
    if (frame_buf_size < required_size) {
        return ERROR_INVALID_PARAM;
    }

    uint32_t offset = 0;

    /* 1. 标识符 (16bit) */
    uint16_t sync = htons(RS422_SYNC_WORD_CMD);
    memcpy(frame_buf + offset, &sync, 2);
    offset += 2;

    /* 2. 包标识 (16bit): 版本号(3bit) + 类型(1bit) + 副导头标识(1bit) + APID(11bit) */
    uint16_t packet_id = (RS422_APID_FILE_DATA & 0x07FF);
    uint16_t packet_id_be = htons(packet_id);
    memcpy(frame_buf + offset, &packet_id_be, 2);
    offset += 2;

    /* 3. 包序列控制 (16bit): 分组标志(2bit) + 源包序列计数(14bit) */
    /* 分组标志基于当前段内的包位置 */
    uint8_t group_flag;
    if (total_packets_in_segment == 1) {
        group_flag = 0x3;  /* 0b11: 单包(当前段只有一个包) */
    } else if (packet_index == 0) {
        group_flag = 0x1;  /* 0b01: 首包(当前段的第一个包) */
    } else if (packet_index == total_packets_in_segment - 1) {
        group_flag = 0x2;  /* 0b10: 尾包(当前段的最后一个包) */
    } else {
        group_flag = 0x0;  /* 0b00: 中间包 */
    }

    /* 源包序列计数使用当前段内的包索引 */
    uint16_t seq_ctrl = (group_flag << 14) | (packet_index & 0x3FFF);
    uint16_t seq_ctrl_be = htons(seq_ctrl);
    memcpy(frame_buf + offset, &seq_ctrl_be, 2);
    offset += 2;

    /* 4. 数据域长度 (16bit): 包数据长度-1 */
    uint16_t data_domain_len = 2 + data_len - 1;  /* segment_num(2) + data - 1 */
    uint16_t data_len_be = htons(data_domain_len);
    memcpy(frame_buf + offset, &data_len_be, 2);
    offset += 2;

    /* 5. 段号 (2字节) */
    uint16_t seg_be = htons(segment_num);
    memcpy(frame_buf + offset, &seg_be, 2);
    offset += 2;

    /* 6. 数据 */
    memcpy(frame_buf + offset, data, data_len);
    offset += data_len;

    /* 7. 校验和 */
    uint16_t checksum = rs422_checksum(frame_buf + 2, offset - 2);
    uint16_t checksum_be = htons(checksum);
    memcpy(frame_buf + offset, &checksum_be, 2);
    offset += 2;

    return offset;
}

int rs422_parse_data_ack(const uint8_t *payload, uint32_t payload_len, uint8_t *result)
{
    if (!payload || payload_len < sizeof(rs422_cmd_data_ack_t) || !result) {
        return ERROR_INVALID_PARAM;
    }

    const rs422_cmd_data_ack_t *ack = (const rs422_cmd_data_ack_t *)payload;
    *result = ack->result;
    return SUCCESS;
}

int rs422_build_transfer_end(uint8_t *frame_buf, uint32_t frame_buf_size)
{
    return rs422_encode_frame(RS422_APID_CONTROL, RS422_CMD_TRANSFER_END,
                              NULL, 0, frame_buf, frame_buf_size);
}

int rs422_parse_transfer_end_ack(const uint8_t *payload, uint32_t payload_len, uint8_t *result)
{
    if (!payload || payload_len < sizeof(rs422_cmd_transfer_end_ack_t) || !result) {
        return ERROR_INVALID_PARAM;
    }

    const rs422_cmd_transfer_end_ack_t *ack = (const rs422_cmd_transfer_end_ack_t *)payload;
    *result = ack->result;
    return SUCCESS;
}

int rs422_build_transfer_abort(uint8_t *frame_buf, uint32_t frame_buf_size)
{
    return rs422_encode_frame(RS422_APID_CONTROL, RS422_CMD_TRANSFER_ABORT,
                              NULL, 0, frame_buf, frame_buf_size);
}

const char* rs422_get_cmd_name(uint16_t cmd_code)
{
    switch (cmd_code) {
        case RS422_CMD_RECONFIG_START:      return "重构启动(1-1)";
        case RS422_CMD_RECONFIG_QUERY:      return "重构查询(1-2)";
        case RS422_CMD_RECONFIG_ACK:        return "重构应答(1-3)";
        case RS422_CMD_VERSION_QUERY:       return "版本查询(1-4)";
        case RS422_CMD_VERSION_ACK:         return "版本应答(1-5)";
        case RS422_CMD_TRANSFER_START:      return "传输开始(3-3)";
        case RS422_CMD_TRANSFER_START_ACK:  return "传输开始应答(3-4)";
        case RS422_CMD_FILE_DATA:           return "文件数据(3-5)";
        case RS422_CMD_DATA_ACK:            return "数据应答(3-6)";
        case RS422_CMD_TRANSFER_END:        return "传输结束(3-7)";
        case RS422_CMD_TRANSFER_END_ACK:    return "传输结束应答(3-8)";
        case RS422_CMD_TRANSFER_ABORT:      return "传输中止(3-9)";
        case RS422_CMD_ABORT_ACK:           return "中止应答(3-10)";
        default:                            return "未知命令";
    }
}

const char* rs422_get_result_desc(uint16_t cmd_code, uint8_t result)
{
    /* 根据命令码和结果码返回描述 */
    // if (result == 0x00) {
    //     return "成功";
    // }

    /* 传输开始应答(3-4)的特殊结果码 */
    if (cmd_code == RS422_CMD_TRANSFER_START_ACK) {
        switch (result) {
            case 0x00: return "正常,可以开始文件传输";
            case 0x11: return "准备中,FPGA正在擦除";
            case 0xFF: return "异常,不能开始文件传输";
            default:   return "未知结果";
        }
    }

    /* 重构应答(1-3)的特殊结果码 */
    if (cmd_code == RS422_CMD_RECONFIG_ACK) {
        switch (result) {
            case 0x00: return "重构成功";
            case 0x11: return "重构中";
            case 0xFF: return "重构失败";
            default:   return "未知结果";
        }
    }

    /* 传输结束应答(3-8)的特殊结果码 */
    if (cmd_code == RS422_CMD_TRANSFER_END_ACK) {
        switch (result) {
            case 0x00: return "文件接收正常";
            case 0x11: return "文件传输异常";
            default:   return "未知结果";
        }
    }

    /* 数据应答(3-6)的结果码 */
    if (cmd_code == RS422_CMD_DATA_ACK) {
        switch (result) {
            case 0x00: return "接收正常";
            case 0xFF: return "接收异常";
            default:   return "未知结果";
        }
    }

    /* 通用结果码 */
    switch (result) {
        case 0x00: return "成功";
        case 0xFF: return "失败";
        default:   return "未知结果";
    }
}
