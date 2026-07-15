#ifndef CHANNEL_SETUP_H
#define CHANNEL_SETUP_H

#include "common.h"
#include "cpri_protocol.h"
#include "fpga_protocol.h"

/* 通道建立原因 */
typedef enum {
    CHANNEL_SETUP_REASON_POWER_ON = 0,
    CHANNEL_SETUP_REASON_RESET = 1,
    CHANNEL_SETUP_REASON_RECONNECT = 2
} channel_setup_reason_t;

/* 生成通道建立请求消息 */
int channel_setup_create_request(cpri_message_t *msg, channel_setup_reason_t reason);

/* 处理通道建立配置消息 */
int channel_setup_handle_config(const cpri_message_t *msg);

/* 生成通道建立响应消息 */
int channel_setup_create_response(cpri_message_t *msg, uint32_t result);

#endif /* CHANNEL_SETUP_H */
