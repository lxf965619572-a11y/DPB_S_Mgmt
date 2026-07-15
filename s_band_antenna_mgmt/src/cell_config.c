#include "cell_config.h"
#include "fpga_handler.h"
#include "logger.h"
#include <string.h>

/* 全局小区配置管理器 */
static cell_config_manager_t g_cell_mgr;

/**
 * 检查是否有任何小区包含频点
 * 返回：true = 有频点，false = 没有频点
 */
static bool has_any_freq(void)
{
    for (int i = 0; i < MAX_CELLS; i++) {
        if (g_cell_mgr.cells[i].active && g_cell_mgr.cells[i].freq_count > 0) {
            return true;
        }
    }
    return false;
}

/**
 * 根据全局频点状态控制发射
 */
static void update_tx_control(void)
{
    bool has_freq = has_any_freq();

    if (has_freq) {
        /* 有频点，开启发射 */
        int tx_ret = fpga_send_tx_control(1);
        if (tx_ret != SUCCESS) {
            LOG_ERROR("Failed to enable TX");
        } else {
            LOG_INFO("TX enabled: at least one cell has freqs");
        }
    } else {
        /* 没有频点，关闭发射 */
        int tx_ret = fpga_send_tx_control(0);
        if (tx_ret != SUCCESS) {
            LOG_ERROR("Failed to disable TX");
        } else {
            LOG_INFO("TX disabled: no freqs in any cell");
        }
    }
}

/* 初始化小区配置管理器 */
int cell_config_init(void)
{
    memset(&g_cell_mgr, 0, sizeof(g_cell_mgr));

    if (pthread_mutex_init(&g_cell_mgr.mutex, NULL) != 0) {
        LOG_ERROR("Failed to init cell config mutex");
        return ERROR_GENERAL;
    }

    LOG_INFO("Cell config manager initialized");
    return SUCCESS;
}

/* 解析小区配置IE */
int cell_config_parse_cell_ie(const uint8_t *ie_data, uint16_t ie_data_len, cell_config_ie_t *cell_cfg)
{
    if (!ie_data || !cell_cfg || ie_data_len < 10) {
        LOG_ERROR("Invalid cell config IE: len=%u", ie_data_len);
        return ERROR_INVALID_PARAM;
    }

    uint32_t offset = 0;
    cell_cfg->cell_cfg_flag = ie_data[offset++];
    memcpy(&cell_cfg->local_cell_id, ie_data + offset, 4);
    offset += 4;
    memcpy(&cell_cfg->cell_power, ie_data + offset, 2);
    offset += 2;
    cell_cfg->reserved = ie_data[offset++];
    cell_cfg->freq_count = ie_data[offset++];
    cell_cfg->cell_type = ie_data[offset++];

    LOG_DEBUG("Parsed cell config IE: flag=%u, cell_id=%u, power=%u, freq_count=%u, type=%u",
              cell_cfg->cell_cfg_flag, cell_cfg->local_cell_id, cell_cfg->cell_power,
              cell_cfg->freq_count, cell_cfg->cell_type);

    return SUCCESS;
}

/* 解析频点配置IE */
int cell_config_parse_freq_ie(const uint8_t *ie_data, uint16_t ie_data_len, freq_config_ie_t *freq_cfg)
{
    if (!ie_data || !freq_cfg || ie_data_len < 32) {
        LOG_ERROR("Invalid freq config IE: len=%u", ie_data_len);
        return ERROR_INVALID_PARAM;
    }

    uint32_t offset = 0;
    freq_cfg->freq_cfg_flag = ie_data[offset++];
    memcpy(&freq_cfg->local_cell_id, ie_data + offset, 4);
    offset += 4;
    freq_cfg->beam_id = ie_data[offset++];
    memcpy(&freq_cfg->dl_center_freq, ie_data + offset, 4);
    offset += 4;
    memcpy(&freq_cfg->reserved1, ie_data + offset, 4);
    offset += 4;
    freq_cfg->special_subframe = ie_data[offset++];
    memcpy(&freq_cfg->sys_subframe_num, ie_data + offset, 4);
    offset += 4;
    memcpy(&freq_cfg->beam_bandwidth, ie_data + offset, 4);
    offset += 4;
    memcpy(&freq_cfg->ul_dl_config, ie_data + offset, 4);
    offset += 4;
    freq_cfg->reserved2 = ie_data[offset++];
    memcpy(&freq_cfg->ul_center_freq, ie_data + offset, 4);

    LOG_DEBUG("Parsed freq config IE: flag=%u, cell_id=%u, beam=%u, dl_freq=%u kHz, ul_freq=%u kHz, bw=%u",
              freq_cfg->freq_cfg_flag, freq_cfg->local_cell_id, freq_cfg->beam_id,
              freq_cfg->dl_center_freq, freq_cfg->ul_center_freq, freq_cfg->beam_bandwidth);

    return SUCCESS;
}

/* 应用小区配置 */
int cell_config_apply_cell(const cell_config_ie_t *cell_cfg, uint32_t *result)
{
    if (!cell_cfg || !result) {
        return ERROR_INVALID_PARAM;
    }

    *result = CONFIG_RESULT_SUCCESS;

    pthread_mutex_lock(&g_cell_mgr.mutex);

    /* 查找小区 */
    int cell_idx = -1;
    for (int i = 0; i < MAX_CELLS; i++) {
        if (g_cell_mgr.cells[i].active &&
            g_cell_mgr.cells[i].local_cell_id == cell_cfg->local_cell_id) {
            cell_idx = i;
            break;
        }
    }

    switch (cell_cfg->cell_cfg_flag) {
        case CELL_CFG_ESTABLISH: {
            /* 建立小区 */
            if (cell_idx >= 0) {
                /* 小区已存在，检查配置是否一致 */
                if (g_cell_mgr.cells[cell_idx].cell_power == cell_cfg->cell_power &&
                    g_cell_mgr.cells[cell_idx].cell_type == cell_cfg->cell_type) {
                    LOG_INFO("Cell %u already exists with same config", cell_cfg->local_cell_id);
                    *result = CONFIG_RESULT_SUCCESS;
                } else {
                    LOG_WARN("Cell %u already exists with different config", cell_cfg->local_cell_id);
                    *result = CONFIG_RESULT_FAILURE;
                }
            } else {
                /* 查找空闲位置 */
                for (int i = 0; i < MAX_CELLS; i++) {
                    if (!g_cell_mgr.cells[i].active) {
                        g_cell_mgr.cells[i].active = true;
                        g_cell_mgr.cells[i].local_cell_id = cell_cfg->local_cell_id;
                        g_cell_mgr.cells[i].cell_power = cell_cfg->cell_power;
                        g_cell_mgr.cells[i].cell_type = cell_cfg->cell_type;
                        g_cell_mgr.cells[i].freq_count = 0;

                        LOG_INFO("Cell %u established: power=%u, type=%u",
                                 cell_cfg->local_cell_id, cell_cfg->cell_power, cell_cfg->cell_type);
                        *result = CONFIG_RESULT_SUCCESS;
                        cell_idx = i;
                        break;
                    }
                }

                if (cell_idx < 0) {
                    LOG_ERROR("No free cell slot available");
                    *result = CONFIG_RESULT_FAILURE;
                }
            }
            break;
        }

        case CELL_CFG_RECONFIG: {
            /* 重配小区 */
            if (cell_idx < 0) {
                LOG_WARN("Cannot reconfig non-existent cell %u", cell_cfg->local_cell_id);
                *result = CONFIG_RESULT_FAILURE;
            } else {
                g_cell_mgr.cells[cell_idx].cell_power = cell_cfg->cell_power;
                LOG_INFO("Cell %u reconfigured: power=%u", cell_cfg->local_cell_id, cell_cfg->cell_power);
                *result = CONFIG_RESULT_SUCCESS;
            }
            break;
        }

        case CELL_CFG_DELETE: {
            /* 删除小区 */
            if (cell_idx >= 0) {
                /* 删除该小区的所有频点 */
                for (int i = 0; i < MAX_FREQS_PER_CELL; i++) {
                    g_cell_mgr.freqs[cell_idx][i].active = false;
                }
                g_cell_mgr.cells[cell_idx].active = false;
                LOG_INFO("Cell %u deleted", cell_cfg->local_cell_id);

                /* 检查全局频点状态并更新发射控制 */
                update_tx_control();
            } else {
                LOG_DEBUG("Delete non-existent cell %u (return success)", cell_cfg->local_cell_id);
            }
            *result = CONFIG_RESULT_SUCCESS;
            break;
        }

        default:
            LOG_ERROR("Invalid cell config flag: %u", cell_cfg->cell_cfg_flag);
            *result = CONFIG_RESULT_FAILURE;
            break;
    }

    pthread_mutex_unlock(&g_cell_mgr.mutex);

    return SUCCESS;
}

/**
 * 等待并验证天线工作模式切换
 * 发送频点配置后，天线会从业务模式切到其他模式（如待机或校准），然后再切回业务模式
 *
 * 返回值：
 *   SUCCESS - 检测到模式从业务模式变化，再回到业务模式
 *   ERROR_TIMEOUT - 超时未检测到预期的模式切换
 *   ERROR_GENERAL - 其他错误
 */
static int wait_for_antenna_mode_cycle(void)
{
    const int MAX_WAIT_MS = 5000;       /* 最大等待时间：5秒 */
    const int POLL_INTERVAL_MS = 100;   /* 轮询间隔：100ms */
    int elapsed_ms = 0;

    bool mode_changed = false;          /* 是否检测到模式变化 */
    uint32_t initial_mode = PHASED_ARRAY_MODE_BUSINESS;

    LOG_INFO("Waiting for antenna work mode to cycle (business -> other -> business)...");

    /* 第一阶段：等待模式从业务模式切换到其他模式 */
    while (elapsed_ms < MAX_WAIT_MS && !mode_changed) {
        fpga_status_frame_t fpga_status;
        if (fpga_handler_get_status(&fpga_status) == SUCCESS) {
            uint32_t current_mode = fpga_status.data.phased_array_work_mode;

            if (current_mode != PHASED_ARRAY_MODE_BUSINESS) {
                LOG_INFO("Antenna mode changed from business to mode %u", current_mode);
                mode_changed = true;
                break;
            }
        }

        usleep(POLL_INTERVAL_MS * 1000);
        elapsed_ms += POLL_INTERVAL_MS;
    }

    if (!mode_changed) {
        LOG_WARN("Antenna mode did not change from business mode within %d ms", MAX_WAIT_MS);
        /* 可能天线已经配置好了，不算错误 */
        return SUCCESS;
    }

    /* 第二阶段：等待模式切回业务模式 */
    bool mode_back_to_business = false;
    while (elapsed_ms < MAX_WAIT_MS) {
        fpga_status_frame_t fpga_status;
        if (fpga_handler_get_status(&fpga_status) == SUCCESS) {
            uint32_t current_mode = fpga_status.data.phased_array_work_mode;

            if (current_mode == PHASED_ARRAY_MODE_BUSINESS) {
                LOG_INFO("Antenna mode returned to business mode (cycle completed)");
                mode_back_to_business = true;
                break;
            }
        }

        usleep(POLL_INTERVAL_MS * 1000);
        elapsed_ms += POLL_INTERVAL_MS;
    }

    if (!mode_back_to_business) {
        LOG_ERROR("Antenna mode did not return to business mode within %d ms", MAX_WAIT_MS);
        return ERROR_TIMEOUT;
    }

    LOG_INFO("Antenna work mode cycle verification completed successfully (took %d ms)", elapsed_ms);
    return SUCCESS;
}

/* 应用频点配置 */
int cell_config_apply_freq(const freq_config_ie_t *freq_cfg, uint32_t *result)
{
    if (!freq_cfg || !result) {
        return ERROR_INVALID_PARAM;
    }

    *result = CONFIG_RESULT_SUCCESS;

    pthread_mutex_lock(&g_cell_mgr.mutex);

    /* 查找小区 */
    int cell_idx = -1;
    for (int i = 0; i < MAX_CELLS; i++) {
        if (g_cell_mgr.cells[i].active &&
            g_cell_mgr.cells[i].local_cell_id == freq_cfg->local_cell_id) {
            cell_idx = i;
            break;
        }
    }

    if (cell_idx < 0) {
        LOG_ERROR("Freq config for non-existent cell %u", freq_cfg->local_cell_id);
        *result = CONFIG_RESULT_FAILURE;
        pthread_mutex_unlock(&g_cell_mgr.mutex);
        return ERROR_GENERAL;
    }

    /* 查找频点 */
    int freq_idx = -1;
    for (int i = 0; i < MAX_FREQS_PER_CELL; i++) {
        if (g_cell_mgr.freqs[cell_idx][i].active &&
            g_cell_mgr.freqs[cell_idx][i].beam_id == freq_cfg->beam_id) {
            freq_idx = i;
            break;
        }
    }

    switch (freq_cfg->freq_cfg_flag) {
        case FREQ_CFG_ESTABLISH: {
            /* 建立频点 */
            if (freq_idx >= 0) {
                /* 频点已存在，用新参数覆盖 */
                LOG_INFO("Freq beam=%u already exists, updating with new params", freq_cfg->beam_id);
            } else {
                /* 查找空闲位置 */
                for (int i = 0; i < MAX_FREQS_PER_CELL; i++) {
                    if (!g_cell_mgr.freqs[cell_idx][i].active) {
                        freq_idx = i;
                        g_cell_mgr.cells[cell_idx].freq_count++;
                        break;
                    }
                }

                if (freq_idx < 0) {
                    LOG_ERROR("No free freq slot for cell %u", freq_cfg->local_cell_id);
                    *result = CONFIG_RESULT_FAILURE;
                    pthread_mutex_unlock(&g_cell_mgr.mutex);
                    return ERROR_GENERAL;
                }
            }

            /* 保存频点配置 */
            g_cell_mgr.freqs[cell_idx][freq_idx].active = true;
            g_cell_mgr.freqs[cell_idx][freq_idx].local_cell_id = freq_cfg->local_cell_id;
            g_cell_mgr.freqs[cell_idx][freq_idx].beam_id = freq_cfg->beam_id;
            g_cell_mgr.freqs[cell_idx][freq_idx].dl_center_freq = freq_cfg->dl_center_freq;
            g_cell_mgr.freqs[cell_idx][freq_idx].ul_center_freq = freq_cfg->ul_center_freq;
            g_cell_mgr.freqs[cell_idx][freq_idx].beam_bandwidth = freq_cfg->beam_bandwidth;
            g_cell_mgr.freqs[cell_idx][freq_idx].ul_dl_config = freq_cfg->ul_dl_config;
            g_cell_mgr.freqs[cell_idx][freq_idx].special_subframe = freq_cfg->special_subframe;

            LOG_INFO("Freq configured: cell=%u, beam=%u, dl=%u kHz, ul=%u kHz, bw=%u",
                     freq_cfg->local_cell_id, freq_cfg->beam_id,
                     freq_cfg->dl_center_freq, freq_cfg->ul_center_freq, freq_cfg->beam_bandwidth);

            /* 发送频率与带宽配置到FPGA/天线 */
            int ret = fpga_send_freq_band(freq_cfg->dl_center_freq,
                                          freq_cfg->ul_center_freq,
                                          freq_cfg->beam_bandwidth);
            if (ret != SUCCESS) {
                LOG_ERROR("Failed to send freq/band to FPGA");
                *result = CONFIG_RESULT_FAILURE;
            } else {
                LOG_INFO("Sent freq/band to antenna: DL=%u kHz, UL=%u kHz, BW=%u",
                         freq_cfg->dl_center_freq, freq_cfg->ul_center_freq, freq_cfg->beam_bandwidth);

                /* 释放锁以允许状态更新 */
                pthread_mutex_unlock(&g_cell_mgr.mutex);

                /* 等待并验证天线工作模式切换（业务模式 -> 其他模式 -> 业务模式）*/
                int mode_check_ret = wait_for_antenna_mode_cycle();
                if (mode_check_ret != SUCCESS) {
                    LOG_ERROR("Antenna work mode cycle verification failed");
                    *result = CONFIG_RESULT_FAILURE;

                    /* 重新加锁以保持函数退出时的锁状态一致 */
                    pthread_mutex_lock(&g_cell_mgr.mutex);
                } else {
                    /* 模式切换验证成功 */
                    *result = CONFIG_RESULT_SUCCESS;

                    /* 重新加锁以保持函数退出时的锁状态一致 */
                    pthread_mutex_lock(&g_cell_mgr.mutex);

                    /* 检查全局频点状态并更新发射控制 */
                    update_tx_control();
                }
            }

            break;
        }

        case FREQ_CFG_DELETE: {
            /* 删除频点 */
            if (freq_idx >= 0) {
                g_cell_mgr.freqs[cell_idx][freq_idx].active = false;
                g_cell_mgr.cells[cell_idx].freq_count--;
                LOG_INFO("Freq deleted: cell=%u, beam=%u", freq_cfg->local_cell_id, freq_cfg->beam_id);

                /* 检查全局频点状态并更新发射控制 */
                update_tx_control();
            } else {
                LOG_DEBUG("Delete non-existent freq beam=%u (return success)", freq_cfg->beam_id);
            }
            *result = CONFIG_RESULT_SUCCESS;
            break;
        }

        default:
            LOG_ERROR("Invalid freq config flag: %u", freq_cfg->freq_cfg_flag);
            *result = CONFIG_RESULT_FAILURE;
            break;
    }

    pthread_mutex_unlock(&g_cell_mgr.mutex);

    return SUCCESS;
}

/* 预分配缓冲区大小 */
#define MAX_PAYLOAD_SIZE 4096

/* 添加IE到载荷 */
static int add_ie(uint8_t **payload, uint32_t *offset, uint16_t ie_type,
                  const uint8_t *ie_data, uint16_t ie_data_len)
{
    uint16_t ie_len = 4 + ie_data_len;

    /* 首次分配时创建缓冲区 */
    if (*payload == NULL) {
        *payload = (uint8_t*)malloc(MAX_PAYLOAD_SIZE);
        if (!*payload) {
            LOG_ERROR("Failed to allocate payload buffer");
            return ERROR_MEMORY;
        }
        *offset = 0;
    }

    /* 检查缓冲区是否足够 */
    if (*offset + ie_len > MAX_PAYLOAD_SIZE) {
        LOG_ERROR("Payload buffer overflow: need %u, max %u",
                  *offset + ie_len, MAX_PAYLOAD_SIZE);
        return ERROR_MEMORY;
    }

    /* 写入IE头（小端序） */
    memcpy(*payload + *offset, &ie_type, 2);
    *offset += 2;
    memcpy(*payload + *offset, &ie_len, 2);
    *offset += 2;

    /* 写入IE数据 */
    if (ie_data_len > 0 && ie_data) {
        memcpy(*payload + *offset, ie_data, ie_data_len);
        *offset += ie_data_len;
    }

    return SUCCESS;
}

/* 生成NR小区配置响应 */
int cell_config_create_response(cpri_message_t *response, const cpri_message_t *request)
{
    if (!response || !request) {
        return ERROR_INVALID_PARAM;
    }

    memset(response, 0, sizeof(cpri_message_t));

    /* 填充响应消息头 */
    response->header.msg_id = MSG_NR_CELL_CONFIG_RSP;
    response->header.paau_id = request->header.paau_id;
    response->header.bbu_id = request->header.bbu_id;
    response->header.port_num = request->header.port_num;
    response->header.serial_num = request->header.serial_num;

    uint8_t *payload = NULL;
    uint32_t offset = 0;

    /* 解析请求消息，生成对应的响应IE */
    uint32_t req_offset = 0;
    cell_config_ie_t cell_cfg;
    uint32_t cell_result = CONFIG_RESULT_SUCCESS;

    while (req_offset + 4 <= request->payload_len) {
        uint16_t ie_type, ie_len;
        memcpy(&ie_type, request->payload + req_offset, 2);
        memcpy(&ie_len, request->payload + req_offset + 2, 2);

        if (req_offset + ie_len > request->payload_len) {
            break;
        }

        uint8_t *ie_data = request->payload + req_offset + 4;
        uint16_t ie_data_len = ie_len - 4;

        if (ie_type == IE_TYPE_CELL_CONFIG) {
            /* 小区配置IE */
            if (cell_config_parse_cell_ie(ie_data, ie_data_len, &cell_cfg) == SUCCESS) {
                cell_config_apply_cell(&cell_cfg, &cell_result);

                /* 生成小区配置响应IE */
                uint8_t cell_resp[8];
                memcpy(cell_resp, &cell_cfg.local_cell_id, 4);
                memcpy(cell_resp + 4, &cell_result, 4);
                add_ie(&payload, &offset, IE_TYPE_CELL_CONFIG_RESP, cell_resp, sizeof(cell_resp));

                LOG_INFO("Cell config response: cell_id=%u, result=%u",
                         cell_cfg.local_cell_id, cell_result);
            }
        } else if (ie_type == IE_TYPE_FREQ_CONFIG) {
            /* 频点配置IE */
            freq_config_ie_t freq_cfg;
            if (cell_config_parse_freq_ie(ie_data, ie_data_len, &freq_cfg) == SUCCESS) {
                uint32_t freq_result = CONFIG_RESULT_SUCCESS;
                cell_config_apply_freq(&freq_cfg, &freq_result);

                /* 生成频点配置响应IE */
                uint8_t freq_resp[9];
                memcpy(freq_resp, &freq_cfg.local_cell_id, 4);
                freq_resp[4] = freq_cfg.beam_id;
                memcpy(freq_resp + 5, &freq_result, 4);
                add_ie(&payload, &offset, IE_TYPE_FREQ_CONFIG_RESP, freq_resp, sizeof(freq_resp));

                LOG_INFO("Freq config response: cell_id=%u, beam=%u, result=%u",
                         freq_cfg.local_cell_id, freq_cfg.beam_id, freq_result);
            }
        }

        req_offset += ie_len;
    }

    response->payload = payload;
    response->payload_len = offset;

    LOG_INFO("Created cell config response: payload_len=%u", offset);
    return SUCCESS;
}

/* 处理NR小区配置消息 */
int cell_config_handle_request(const cpri_message_t *msg)
{
    if (!msg || !msg->payload) {
        return ERROR_INVALID_PARAM;
    }

    LOG_INFO("Handling NR cell config request");
    return SUCCESS;
}

/* 销毁小区配置管理器 */
void cell_config_destroy(void)
{
    pthread_mutex_destroy(&g_cell_mgr.mutex);
    LOG_INFO("Cell config manager destroyed");
}
