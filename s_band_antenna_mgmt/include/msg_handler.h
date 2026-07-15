#ifndef MSG_HANDLER_H
#define MSG_HANDLER_H

#include "common.h"
#include "cpri_protocol.h"

/* 消息处理器初始化 */
int msg_handler_init(void);

/* 消息分发处理 */
int msg_handler_dispatch(const cpri_message_t *msg);

/* 消息处理器销毁 */
void msg_handler_destroy(void);

/* 具体消息处理函数（由各业务模块实现） */
int handle_channel_setup_req(const cpri_message_t *msg);
int handle_heartbeat(const cpri_message_t *msg);
int handle_status_query(const cpri_message_t *msg);
int handle_param_query(const cpri_message_t *msg);
int handle_param_config(const cpri_message_t *msg);
int handle_cell_config(const cpri_message_t *msg);
int handle_log_upload_req(const cpri_message_t *msg);
int handle_version_download_req(const cpri_message_t *msg);
int handle_version_activate_ind(const cpri_message_t *msg);
int handle_alarm_query_req(const cpri_message_t *msg);
int handle_loopback_req(const cpri_message_t *msg);
int handle_reset_ind(const cpri_message_t *msg);
int handle_remote_reset_ind(const cpri_message_t *msg);
int handle_transparent_msg(const cpri_message_t *msg);
int handle_phased_array_calib_req(const cpri_message_t *msg);

#endif /* MSG_HANDLER_H */
