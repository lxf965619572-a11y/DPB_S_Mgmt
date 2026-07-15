#ifndef FPGA_TO_CPRI_H
#define FPGA_TO_CPRI_H

#include "common.h"
#include "fpga_protocol.h"
#include "cpri_protocol.h"

/* 将FPGA状态转换为CPRI状态查询响应 */
int fpga_to_cpri_status_response(const fpga_status_frame_t *fpga_status, cpri_message_t *cpri_msg);

/* 将FPGA状态转换为CPRI参数查询响应 */
int fpga_to_cpri_param_response(const fpga_status_frame_t *fpga_status, cpri_message_t *cpri_msg);

/* 将FPGA告警转换为CPRI告警上报 */
int fpga_to_cpri_alarm_report(const fpga_status_frame_t *fpga_status, cpri_message_t *cpri_msg);

#endif /* FPGA_TO_CPRI_H */
