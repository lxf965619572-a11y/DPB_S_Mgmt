/**
 * @file rs422_protocol.h
 * @brief RS-422协议层 - 用于FPGA固件上注
 *
 * 实现QNIXY32110-2023RS-422接口使用规范中定义的指令集
 * 通过UART (ttyAMA2)上传FPGA固件
 */

#ifndef RS422_PROTOCOL_H
#define RS422_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include "common.h"

/* ========== 指令码定义 ========== */

/* 重构控制指令 (1-1 到 1-3) */
#define RS422_CMD_RECONFIG_START        0x01AF  /* 1-1: 启动重构 */
#define RS422_CMD_RECONFIG_QUERY        0x01C5  /* 1-2: 查询重构结果 */
#define RS422_CMD_RECONFIG_ACK          0x01CA  /* 1-3: 重构结果应答 */

/* 版本查询指令 (1-4 到 1-5) */
#define RS422_CMD_VERSION_QUERY         0x0101  /* 1-4: 查询固件版本 */
#define RS422_CMD_VERSION_ACK           0x0108  /* 1-5: 版本应答 */

/* 文件传输指令 (3-3 到 3-10) */
#define RS422_CMD_TRANSFER_START        0x0155  /* 3-3: 文件传输开始 */
#define RS422_CMD_TRANSFER_START_ACK    0x015A  /* 3-4: 传输开始应答 */
#define RS422_CMD_FILE_DATA             0x0180  /* 3-5: 文件数据帧 */
#define RS422_CMD_DATA_ACK              0x018A  /* 3-6: 数据接收应答 */
#define RS422_CMD_TRANSFER_END          0x01AA  /* 3-7: 文件传输结束 */
#define RS422_CMD_TRANSFER_END_ACK      0x01BB  /* 3-8: 传输结束应答 */
#define RS422_CMD_TRANSFER_ABORT        0x018F  /* 3-9: 传输异常中止 */
#define RS422_CMD_ABORT_ACK             0x018E  /* 3-10: 中止应答 */

/* ========== 结果码定义 ========== */

/* 重构结果 (1-3 响应) */
#define RS422_RECONFIG_SUCCESS          0x00    /* 重构成功 */
#define RS422_RECONFIG_IN_PROGRESS      0x11    /* 重构中 */
#define RS422_RECONFIG_NOT_STARTED      0x22    /* 未收到启动指令 */
#define RS422_RECONFIG_FAILED           0xFF    /* 重构失败 */

/* 传输开始应答 (3-4 响应) */
#define RS422_TRANSFER_READY            0x00    /* 准备就绪 */
#define RS422_TRANSFER_PREPARING        0x11    /* 准备中(继续轮询) */
#define RS422_TRANSFER_REJECTED         0xFF    /* 拒绝/异常 */

/* 数据应答 (3-6 响应) */
#define RS422_DATA_RECEIVED_OK          0x00    /* 接收正常 */
#define RS422_DATA_RECEIVED_ERROR       0xFF    /* 接收异常 */

/* 传输结束应答 (3-8 响应) */
#define RS422_TRANSFER_COMPLETE_OK      0x00    /* 文件接收正常 */
#define RS422_TRANSFER_COMPLETE_ERROR   0x11    /* 文件传输异常 */

/* ========== APID定义 ========== */

/* 控制帧APID (默认值,用于命令) */
#define RS422_APID_CONTROL              0x0180  /* 0011101 0000 (示例) */

/* 数据帧APID (低4位必须为0xF,用于文件数据帧) */
#define RS422_APID_FILE_DATA            0x018F  /* 数据帧APID (低4位为0xF) */
#define RS422_APID_DATA_MASK            0x000F  /* 低4位掩码 */
#define RS422_APID_DATA_MARKER          0x000F  /* 数据帧标记(必须为0xF) */

/* ========== 文件类型定义 ========== */

#define RS422_FILE_TYPE_MIN             0xF0    /* 最小文件类型 */
#define RS422_FILE_TYPE_MAX             0xFF    /* 最大文件类型 */
#define RS422_FILE_TYPE_DEFAULT         0xFF    /* 默认文件类型 */

/* ========== 帧结构 ========== */

/* RS-422帧头 (简化版,实际结构见PDF规范) */
typedef struct __attribute__((packed)) {
    uint16_t sync;              /* 同步字 (如 0xEB90) */
    uint16_t apid;              /* 应用标识 */
    uint16_t seq_count;         /* 序列计数器 */
    uint16_t data_len;          /* 数据长度(载荷) */
} rs422_frame_header_t;

/* 最大帧大小 */
#define RS422_MAX_FRAME_SIZE            1012    /* 最大帧大小 */
#define RS422_MAX_PAYLOAD_SIZE          1002    /* 最大载荷(帧-头-校验和) */
#define RS422_HEADER_SIZE               8       /* 帧头大小 */
#define RS422_CRC_SIZE                  2       /* 校验和大小(保持宏名兼容) */
#define RS422_CHECKSUM_SIZE             2       /* 校验和大小 */

/* ========== 命令结构 ========== */

/**
 * 命令1-1: 启动重构
 */
typedef struct __attribute__((packed)) {
    uint8_t file_type;          /* 文件类型: 0xF0-0xFF */
    uint8_t file_sub_type;      /* 文件子类型: 0x00-0xFF */
} rs422_cmd_reconfig_start_t;

/**
 * 命令1-3: 重构结果应答 (响应)
 */
typedef struct __attribute__((packed)) {
    uint8_t result;             /* 结果码 */
} rs422_cmd_reconfig_ack_t;

/**
 * 命令1-5: 版本应答 (响应)
 */
typedef struct __attribute__((packed)) {
    char version[32];           /* 固件版本字符串(ASCII) */
} rs422_cmd_version_ack_t;

/**
 * 命令3-3: 文件传输开始
 */
typedef struct __attribute__((packed)) {
    uint8_t device_id;          /* 设备标识(APID高7位) */
    uint8_t file_type;          /* 文件类型: 0xF0-0xFF */
    uint8_t file_sub_type;      /* 文件子类型 */
    uint16_t segment_info;      /* Bit[15:14]=是否分段(0=否,3=是), Bit[13:0]=段数 */
    uint32_t file_length;       /* 文件总长度或段长度 */
    uint32_t last_segment_len;  /* 最后一段长度 */
    uint16_t file_checksum;     /* 整个文件的CRC16-CCITT-FALSE */
} rs422_cmd_transfer_start_t;

/**
 * 命令3-4: 传输开始应答 (响应)
 */
typedef struct __attribute__((packed)) {
    uint8_t result;             /* 结果码: 0x00=正常, 0x11=准备中(擦除), 0xFF=异常 */
} rs422_cmd_transfer_start_ack_t;

/**
 * 命令3-5: 文件数据帧 (特殊APID)
 */
typedef struct __attribute__((packed)) {
    uint16_t segment_num;       /* 段号: 0x0000-0x3FFF */
    uint8_t data[];             /* 文件数据(可变长度) */
} rs422_cmd_file_data_t;

/**
 * 命令3-6: 数据接收应答 (响应)
 */
typedef struct __attribute__((packed)) {
    uint8_t result;             /* 结果码 */
} rs422_cmd_data_ack_t;

/**
 * 命令3-8: 传输结束应答 (响应)
 */
typedef struct __attribute__((packed)) {
    uint8_t result;             /* 结果码 */
} rs422_cmd_transfer_end_ack_t;

/* ========== API函数 ========== */

/**
 * @brief 初始化RS-422协议层
 * @return 成功返回SUCCESS,失败返回错误码
 */
int rs422_protocol_init(void);

/**
 * @brief 清理RS-422协议层
 */
void rs422_protocol_cleanup(void);

/**
 * @brief 计算校验和(单字节累加求和取反,取低16位) - 用于帧校验
 * @param data 数据缓冲区
 * @param len 数据长度
 * @return 校验和值(16位)
 */
uint16_t rs422_checksum(const uint8_t *data, uint32_t len);

/**
 * @brief 计算CRC16-CCITT-FALSE - 用于文件校验
 * @param data 数据缓冲区
 * @param len 数据长度
 * @return CRC16值(16位)
 */
uint16_t rs422_crc16_ccitt_false(const uint8_t *data, uint32_t len);

/**
 * @brief 编码RS-422帧
 * @param apid 应用标识
 * @param cmd_code 命令码
 * @param payload 命令载荷
 * @param payload_len 载荷长度
 * @param frame_buf 输出:帧缓冲区
 * @param frame_buf_size 缓冲区大小
 * @return 成功返回帧长度,失败返回负数错误码
 */
int rs422_encode_frame(uint16_t apid, uint16_t cmd_code,
                       const uint8_t *payload, uint32_t payload_len,
                       uint8_t *frame_buf, uint32_t frame_buf_size);

/**
 * @brief 解码RS-422帧
 * @param frame_buf 帧缓冲区
 * @param frame_len 帧长度
 * @param apid 输出:应用标识
 * @param cmd_code 输出:命令码
 * @param payload 输出:载荷指针(指向frame_buf内部)
 * @param payload_len 输出:载荷长度
 * @return 成功返回SUCCESS,失败返回错误码
 */
int rs422_decode_frame(const uint8_t *frame_buf, uint32_t frame_len,
                       uint16_t *apid, uint16_t *cmd_code,
                       const uint8_t **payload, uint32_t *payload_len);

/**
 * @brief 构造重构启动命令(1-1)
 * @param file_type 文件类型
 * @param file_sub_type 文件子类型
 * @param frame_buf 输出:帧缓冲区
 * @param frame_buf_size 缓冲区大小
 * @return 成功返回帧长度,失败返回负数错误码
 */
int rs422_build_reconfig_start(uint8_t file_type, uint8_t file_sub_type,
                                uint8_t *frame_buf, uint32_t frame_buf_size);

/**
 * @brief 构造重构查询命令(1-2)
 * @param frame_buf 输出:帧缓冲区
 * @param frame_buf_size 缓冲区大小
 * @return 成功返回帧长度,失败返回负数错误码
 */
int rs422_build_reconfig_query(uint8_t *frame_buf, uint32_t frame_buf_size);

/**
 * @brief 解析重构应答(1-3)
 * @param payload 响应载荷
 * @param payload_len 载荷长度
 * @param result 输出:结果码
 * @return 成功返回SUCCESS,失败返回错误码
 */
int rs422_parse_reconfig_ack(const uint8_t *payload, uint32_t payload_len, uint8_t *result);

/**
 * @brief 构造版本查询命令(1-4)
 * @param frame_buf 输出:帧缓冲区
 * @param frame_buf_size 缓冲区大小
 * @return 成功返回帧长度,失败返回负数错误码
 */
int rs422_build_version_query(uint8_t *frame_buf, uint32_t frame_buf_size);

/**
 * @brief 解析版本应答(1-5)
 * @param payload 响应载荷
 * @param payload_len 载荷长度
 * @param version 输出:版本字符串(32字节)
 * @return 成功返回SUCCESS,失败返回错误码
 */
int rs422_parse_version_ack(const uint8_t *payload, uint32_t payload_len, char *version);

/**
 * @brief 构造传输开始命令(3-3)
 * @param params 传输参数
 * @param frame_buf 输出:帧缓冲区
 * @param frame_buf_size 缓冲区大小
 * @return 成功返回帧长度,失败返回负数错误码
 */
int rs422_build_transfer_start(const rs422_cmd_transfer_start_t *params,
                                uint8_t *frame_buf, uint32_t frame_buf_size);

/**
 * @brief 解析传输开始应答(3-4)
 * @param payload 响应载荷
 * @param payload_len 载荷长度
 * @param result 输出:结果码
 * @return 成功返回SUCCESS,失败返回错误码
 */
int rs422_parse_transfer_start_ack(const uint8_t *payload, uint32_t payload_len, uint8_t *result);

/**
 * @brief 构造文件数据帧(3-5)
 * @param segment_num 段号(1MB段编号)
 * @param data 文件数据
 * @param data_len 数据长度
 * @param frame_buf 输出:帧缓冲区
 * @param frame_buf_size 缓冲区大小
 * @param packet_index 当前段内的包索引(0表示首包)
 * @param total_packets_in_segment 当前段内的总包数
 * @return 成功返回帧长度,失败返回负数错误码
 */
int rs422_build_file_data(uint16_t segment_num, const uint8_t *data, uint32_t data_len,
                           uint8_t *frame_buf, uint32_t frame_buf_size,
                           uint32_t packet_index, uint32_t total_packets_in_segment);

/**
 * @brief 解析数据应答(3-6)
 * @param payload 响应载荷
 * @param payload_len 载荷长度
 * @param result 输出:结果码
 * @return 成功返回SUCCESS,失败返回错误码
 */
int rs422_parse_data_ack(const uint8_t *payload, uint32_t payload_len, uint8_t *result);

/**
 * @brief 构造传输结束命令(3-7)
 * @param frame_buf 输出:帧缓冲区
 * @param frame_buf_size 缓冲区大小
 * @return 成功返回帧长度,失败返回负数错误码
 */
int rs422_build_transfer_end(uint8_t *frame_buf, uint32_t frame_buf_size);

/**
 * @brief 解析传输结束应答(3-8)
 * @param payload 响应载荷
 * @param payload_len 载荷长度
 * @param result 输出:结果码
 * @return 成功返回SUCCESS,失败返回错误码
 */
int rs422_parse_transfer_end_ack(const uint8_t *payload, uint32_t payload_len, uint8_t *result);

/**
 * @brief 构造传输中止命令(3-9)
 * @param frame_buf 输出:帧缓冲区
 * @param frame_buf_size 缓冲区大小
 * @return 成功返回帧长度,失败返回负数错误码
 */
int rs422_build_transfer_abort(uint8_t *frame_buf, uint32_t frame_buf_size);

/**
 * @brief 获取命令名称字符串
 * @param cmd_code 命令码
 * @return 命令名称
 */
const char* rs422_get_cmd_name(uint16_t cmd_code);

/**
 * @brief 获取结果码描述
 * @param cmd_code 命令码(用于确定上下文)
 * @param result 结果码
 * @return 结果描述
 */
const char* rs422_get_result_desc(uint16_t cmd_code, uint8_t result);

/**
 * @brief 回滚序列计数器(用于重发时保持序列号不变)
 */
void rs422_seq_counter_rollback(void);

/**
 * @brief 重置序列计数器为0(用于开始新文件传输)
 */
void rs422_seq_counter_reset(void);

#endif /* RS422_PROTOCOL_H */
