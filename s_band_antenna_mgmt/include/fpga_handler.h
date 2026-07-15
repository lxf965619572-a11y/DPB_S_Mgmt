#ifndef FPGA_HANDLER_H
#define FPGA_HANDLER_H

#include "common.h"
#include "fpga_protocol.h"

/* FPGA状态管理结构 */
typedef struct {
    fpga_status_frame_t current_status;
    bool status_valid;
    pthread_mutex_t status_mutex;
    time_t last_update_time;
} fpga_state_manager_t;

/* 初始化FPGA处理器 */
int fpga_handler_init(void);

/* 处理FPGA状态更新 */
int fpga_handler_on_status_update(const fpga_status_frame_t *status);

/* 获取当前FPGA状态 */
int fpga_handler_get_status(fpga_status_frame_t *status);

/* 获取光纤端口信息 */
int fpga_handler_get_fiber_ports(uint8_t *main_port, uint8_t *sub_port);

/* 发送FPGA消息的便捷函数 */
int fpga_send_cpri_mode(uint32_t mode);
int fpga_send_cpri_config(uint32_t config);
int fpga_send_beam_num(uint8_t cmd_beam_num, uint8_t service_beam_num);
int fpga_send_phase_restore(uint32_t restore_type);
int fpga_send_calibration(uint32_t calib_params);
int fpga_send_loopback(uint8_t loopback_type);
int fpga_send_freq_band(uint32_t dl_freq, uint32_t ul_freq, uint32_t bandwidth);
int fpga_send_system_time(void);
int fpga_send_tx_control(uint8_t tx_enable);
int fpga_send_power_off_ack(void);

/* 设置CPRI工作模式 (用于参数配置) */
int fpga_handler_set_work_mode(uint32_t work_mode);

/* 销毁FPGA处理器 */
void fpga_handler_destroy(void);

/* 安全关机流程 */
void fpga_safe_shutdown(void);

/* 全局FPGA状态管理器 */
extern fpga_state_manager_t g_fpga_state;

#endif /* FPGA_HANDLER_H */
