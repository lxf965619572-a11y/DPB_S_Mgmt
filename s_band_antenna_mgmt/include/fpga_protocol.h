#ifndef FPGA_PROTOCOL_H
#define FPGA_PROTOCOL_H

#include "common.h"
#include <arpa/inet.h>

/* FPGA帧格式定义 */
#define FPGA_FRAME_HEADER       0xEB90  /* 帧头（大端） */
#define FPGA_FRAME_TAIL         0x5555  /* 帧尾（大端） */
#define FPGA_HEADER_SIZE        2       /* 帧头2字节 */
#define FPGA_MSG_ID_SIZE        1       /* 消息ID 1字节 */
#define FPGA_LENGTH_SIZE        1       /* 长度字段1字节 */
#define FPGA_TAIL_SIZE          2       /* 帧尾2字节 */
#define FPGA_MIN_FRAME_SIZE     6       /* 帧头(2)+消息ID(1)+长度(1)+帧尾(2) */

/* FPGA消息ID定义 */
typedef enum {
    FPGA_MSG_CPRI_MODE = 0x01,          /* 相控阵工作模式 */
    FPGA_MSG_CPRI_CONFIG = 0x02,        /* CPRI工作模式 */
    FPGA_MSG_CMD_BEAM_NUM = 0x03,       /* 指令波束数 */
    FPGA_MSG_SERVICE_BEAM_NUM = 0x04,   /* 业务波束数 */
    FPGA_MSG_PHASE_RESTORE = 0x05,      /* 相控阵复位 */
    FPGA_MSG_CALIBRATION = 0x06,        /* 校准指示 */
    FPGA_MSG_LOOPBACK = 0x07,           /* 环回控制 */
    FPGA_MSG_FREQ_BAND = 0x08,          /* 频率与带宽 */
    FPGA_MSG_WAVE_TIMES = 0x09,         /* 波束次数 */
    FPGA_MSG_SYSTEM_TIME = 0x0A,        /* 系统时间 */
    FPGA_MSG_TX_CONTROL = 0x0B,         /* 发射控制 */
    FPGA_MSG_POWER_OFF_ACK = 0x0D,      /* 断电应答 */
    FPGA_MSG_LOG_QUERY = 0x11,          /* 日志查询 */
    FPGA_MSG_PASSTHROUGH = 0x12,        /* 透传数据 */
    FPGA_MSG_STATUS_QUERY = 0xFF,       /* 状态检询 */
} fpga_msg_id_t;

/* FPGA状态响应帧结构（133字节总长度）
 * 帧格式：帧头(2) + 长度(2) + 数据(127) + 帧尾(2) = 133字节
 * 本结构体仅定义数据部分（127字节）
 * 严格按照FPGA_Telemetry_Definition.md定义
 */
typedef struct __attribute__((packed)) {
    /* Byte 0: 通道建立标志 */
    uint8_t  link_success_flag;         /* Bit0-3: 主路0-3建链成功, Bit4-7: 备路0-3建链成功 */

    /* Bytes 1-3: 光纤和建立原因 */
    uint8_t  main_fiber_num;            /* 主光纤号 */
    uint8_t  backup_fiber_num;          /* 备光纤号 */
    uint8_t  channel_setup_reason;      /* 通道建立原因 */

    /* Bytes 4-7: NR波束数量 */
    uint32_t nr_beam_count;             /* 支持的NR波束数量 */

    /* Bytes 8-11: 功率和模式 */
    uint16_t max_tx_power;              /* 最大发射功率（1/256 dBm） */
    uint16_t supported_modes;           /* 支持的模式 */

    /* Bytes 12-43: 硬件类型 */
    uint8_t  hw_type[32];               /* 相控阵硬件类型（ASCII字符串） */

    /* Bytes 44-47: 硬件版本 */
    uint32_t hw_version;                /* 相控阵硬件版本号 */

    /* Bytes 48-49: 当前功率 */
    uint16_t current_output_power;      /* 当前功率值（1/256 dBm） */

    /* Byte 50: 断电通知 */
    uint8_t  power_off_notify;          /* 断电通知（1=有效，0=无效） */

    /* Byte 51: 保留 */
    uint8_t  reserved1;

    /* Bytes 52-53: 波束个数 */
    uint8_t  cmd_beam_num;              /* 信令波束个数 n1 */
    uint8_t  service_beam_num;          /* 业务波束个数 n2 */

    /* Bytes 54-57: 波束状态 */
    uint32_t beam_status;               /* 波束状态位图（低16位对应16个物理波束） */

    /* Bytes 58-61: 本振频率 */
    uint32_t lo_frequency;              /* 本振频率（编码值） */

    /* Bytes 62-65: 本振状态 */
    uint32_t lo_lock_status;            /* 本振状态（0=锁定，1=失锁） */

    /* Bytes 66-71: 硬件类型补充 */
    uint8_t  hw_type_ext[6];            /* 相控阵硬件类型(回件) */

    /* Byte 72: 保留 */
    uint8_t  reserved2;

    /* Bytes 73-76: 时钟状态 */
    uint32_t clock_sync_status;         /* 时钟状态（0=同步，1=失锁） */

    /* Bytes 77-80: 相控阵工作模式 */
    uint32_t phased_array_work_mode;    /* 相控阵工作模式（0=业务，1=频谱，2=待机，3=自校准） */

    /* Bytes 81-84: CPRI工作模式 */
    uint32_t cpri_work_mode;            /* CPRI工作模式（1=普通，2=级联，3=主备，4=反向分担） */

    /* Byte 85: 校准结果 */
    uint8_t  calib_result;              /* 校准结果（0=成功，其他=失败） */

    /* Byte 86: 保留 */
    uint8_t  reserved3;

    /* Bytes 87-98: CPRI时延参数 */
    uint32_t toffset;                   /* CPRI Toffset数值 */
    uint32_t t2a;                       /* CPRI T2a数值 */
    uint32_t ta3;                       /* CPRI Ta3数值 */

    /* Byte 99: 回环回路结果 */
    uint8_t  loopback_result;           /* 回环回路结果（低4位有效） */

    /* Bytes 100-102: 保留 */
    uint8_t  reserved4[3];

    /* Byte 103: 通道故障数量 */
    uint8_t  channel_fault_count;       /* 通道故障数量 */

    /* Bytes 104-112: 温度检测 */
    int8_t   temp1;                     /* 温度检测点1的温度（℃，有符号） */
    int8_t   temp2;                     /* 温度检测点2的温度（℃，有符号） */
    int8_t   temp3;                     /* 温度检测点3的温度（℃，有符号） */
    int8_t   temp1_high;                /* 温度检测点1的上门限 */
    int8_t   temp1_low;                 /* 温度检测点1的下门限 */
    int8_t   temp2_high;                /* 温度检测点2的上门限 */
    int8_t   temp2_low;                 /* 温度检测点2的下门限 */
    int8_t   temp3_high;                /* 温度检测点3的上门限 */
    int8_t   temp3_low;                 /* 温度检测点3的下门限 */

    /* Bytes 113-122: 频段信息（预留，CPU写死） */
    uint16_t tx_freq_start;             /* 发送频段起始频点 */
    uint16_t tx_freq_end;               /* 发送频段截止频点 */
    uint16_t rx_freq_start;             /* 接收频段起始频点 */
    uint16_t rx_freq_end;               /* 接收频段截止频点 */
    uint16_t beam_bandwidth;            /* 支持的波束带宽 */

    /* Bytes 123-126: 保留 */
    uint32_t reserved5;
} fpga_status_data_t;

/* 完整的FPGA状态响应帧（133字节）
 * 注意：FPGA回复的遥测数据没有消息ID字段！
 * 格式：帧头(2) + 长度(2) + 数据(127) + 帧尾(2) = 133字节
 */
typedef struct __attribute__((packed)) {
    uint16_t frame_header;              /* 0xEB90 (大端) */
    uint16_t length;                    /* 长度字段 = 127 (2字节，大端) */
    fpga_status_data_t data;            /* 状态数据（127字节） */
    uint16_t frame_tail;                /* 0x5555 (大端) */
} fpga_status_frame_t;

/* FPGA消息帧通用结构 */
typedef struct {
    uint8_t msg_id;
    uint8_t *payload;
    uint32_t payload_len;
} fpga_message_t;

/* 频率编解码函数 */
uint32_t fpga_encode_frequency(uint32_t freq_khz);
uint32_t fpga_decode_frequency(uint32_t encoded_freq);

/* FPGA消息编解码函数 */
int fpga_encode_message(const fpga_message_t *msg, uint8_t *buffer, uint32_t buffer_size);
int fpga_decode_message(const uint8_t *buffer, uint32_t buffer_len, fpga_message_t *msg);
int fpga_decode_status_frame(const uint8_t *buffer, uint32_t buffer_len, fpga_status_frame_t *status);
void fpga_free_message(fpga_message_t *msg);

/* 字节序转换宏（大端） */
#define HTONS(x) htons(x)
#define HTONL(x) htonl(x)
#define NTOHS(x) ntohs(x)
#define NTOHL(x) ntohl(x)

#endif /* FPGA_PROTOCOL_H */
