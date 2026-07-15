#include "status_query.h"
#include "fpga_handler.h"
#include "logger.h"
#include <string.h>

/* 预分配缓冲区大小 */
#define MAX_PAYLOAD_SIZE 4096

/* 添加IE到载荷 */
static int add_ie(uint8_t **payload, uint32_t *offset, uint16_t ie_type,
                  const uint8_t *ie_data, uint16_t ie_data_len)
{
    uint16_t ie_len = 4 + ie_data_len;  /* IE头(4字节) + 数据 */

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

int status_query_create_response(cpri_message_t *response, const cpri_message_t *request)
{
    if (!response || !request) {
        return ERROR_INVALID_PARAM;
    }

    memset(response, 0, sizeof(cpri_message_t));

    /* 填充响应消息头 */
    response->header.msg_id = MSG_PAAU_STATUS_QUERY_RSP;
    response->header.paau_id = request->header.paau_id;
    response->header.bbu_id = request->header.bbu_id;
    response->header.port_num = request->header.port_num;
    response->header.serial_num = request->header.serial_num;  /* 带回相同的流水号 */

    /* 获取FPGA状态 */
    fpga_status_frame_t fpga_status;
    int has_fpga_status = (fpga_handler_get_status(&fpga_status) == SUCCESS);

    uint8_t *payload = NULL;
    uint32_t offset = 0;

    /* 解析请求中的IE，根据查询类型生成响应 */
    uint32_t req_offset = 0;
    while (req_offset + 4 <= request->payload_len) {
        uint16_t ie_type, ie_len;
        memcpy(&ie_type, request->payload + req_offset, 2);
        memcpy(&ie_len, request->payload + req_offset + 2, 2);

        if (req_offset + ie_len > request->payload_len) {
            break;
        }

        uint8_t *ie_data = request->payload + req_offset + 4;
        uint16_t ie_data_len = ie_len - 4;

        switch (ie_type) {
            case IE_TYPE_BEAM_STATUS_QUERY: {
                /* 波束状态查询 */
                uint16_t beam_number = 0xFFFF;  /* 默认查询所有波束 */
                if (ie_data_len >= 2) {
                    memcpy(&beam_number, ie_data, 2);
                }

                if (beam_number == 0xFFFF) {
                    /* 查询所有波束：为每个波束返回一个独立的 IE */
                    LOG_INFO("Query all beams status");

                    /* 假设支持 16 个波束（根据实际硬件配置调整） */
                    uint16_t max_beams = 16;
                    if (has_fpga_status) {
                        /* 可以从 FPGA 状态获取实际波束数量 */
                        /* max_beams = fpga_status.data.beam_count; */
                    }

                    for (uint16_t i = 0; i < max_beams; i++) {
                        uint8_t beam_resp[6];
                        beam_resp[0] = 0;  /* 保留 */
                        beam_resp[1] = (uint8_t)i;  /* 波束号 */

                        /* 从FPGA状态获取该波束的状态 */
                        /* FPGA: bit=1表示使能; 响应IE: 0=使能,1=不使能，需要取反 */
                        uint32_t beam_status = 1;  /* 默认不使能 */
                        if (has_fpga_status) {
                            /* 从 beam_status 位图中提取对应位并取反 */
                            uint32_t fpga_bit = (fpga_status.data.beam_status >> i) & 0x01;
                            beam_status = fpga_bit ? 0 : 1;  /* FPGA bit=1 → 响应0(使能) */
                        }
                        memcpy(beam_resp + 2, &beam_status, 4);

                        add_ie(&payload, &offset, IE_TYPE_BEAM_STATUS_RESP, beam_resp, sizeof(beam_resp));
                    }
                    LOG_INFO("Added %u beam status responses", max_beams);
                } else {
                    /* 查询单个波束 */
                    uint8_t beam_resp[6];
                    beam_resp[0] = 0;  /* 保留 */
                    beam_resp[1] = (uint8_t)beam_number;

                    /* 从FPGA状态获取波束状态 */
                    /* FPGA: bit=1表示使能; 响应IE: 0=使能,1=不使能，需要取反 */
                    uint32_t beam_status = 1;  /* 默认不使能 */
                    if (has_fpga_status && beam_number < 16) {
                        uint32_t fpga_bit = (fpga_status.data.beam_status >> beam_number) & 0x01;
                        beam_status = fpga_bit ? 0 : 1;  /* FPGA bit=1 → 响应0(使能) */
                    }
                    memcpy(beam_resp + 2, &beam_status, 4);

                    add_ie(&payload, &offset, IE_TYPE_BEAM_STATUS_RESP, beam_resp, sizeof(beam_resp));
                    LOG_DEBUG("Added beam status response: beam=%u, status=%u", beam_number, beam_status);
                }
                break;
            }

            case IE_TYPE_LO_STATUS_QUERY: {
                /* 本振状态查询 */
                uint8_t lo_resp[8];

                if (has_fpga_status) {
                    /* 从FPGA状态获取本振频率和锁定状态 */
                    uint32_t lo_freq_khz = fpga_decode_frequency(fpga_status.data.lo_frequency);
                    uint32_t lo_freq_100khz = lo_freq_khz / 100;  /* 转换为100kHz单位 */
                    memcpy(lo_resp, &lo_freq_100khz, 4);

                    uint32_t lo_status = fpga_status.data.lo_lock_status;  /* 0=锁定, 非0=失锁 */
                    memcpy(lo_resp + 4, &lo_status, 4);

                    //LOG_DEBUG("Added LO status response: freq=%u (100kHz), status=%u", lo_freq_100khz, lo_status);
                } else {
                    /* FPGA状态未就绪，使用默认值 */
                    uint32_t default_freq = 21000;  /* 2.1 GHz */
                    uint32_t default_status = 0;    /* 锁定 */
                    memcpy(lo_resp, &default_freq, 4);
                    memcpy(lo_resp + 4, &default_status, 4);
                }

                add_ie(&payload, &offset, IE_TYPE_LO_STATUS_RESP, lo_resp, sizeof(lo_resp));
                break;
            }

            case IE_TYPE_CLOCK_STATUS_QUERY: {
                /* 时钟状态查询 */
                uint8_t clock_resp[4];

                if (has_fpga_status) {
                    uint32_t clock_status = fpga_status.data.clock_sync_status;  /* 0=同步, 非0=失步 */
                    memcpy(clock_resp, &clock_status, 4);
                    //LOG_DEBUG("Added clock status response: status=%u", clock_status);
                } else {
                    uint32_t default_status = 0;  /* 同步 */
                    memcpy(clock_resp, &default_status, 4);
                }

                add_ie(&payload, &offset, IE_TYPE_CLOCK_STATUS_RESP, clock_resp, sizeof(clock_resp));
                break;
            }

            case IE_TYPE_RUN_STATUS_QUERY: {
                /* 相控阵运行状态查询 */
                uint8_t run_resp[4];

                if (has_fpga_status) {
                    uint32_t run_status = fpga_status.data.phased_array_work_mode;
                    memcpy(run_resp, &run_status, 4);
                    //LOG_DEBUG("Added run status response: status=%u", run_status);
                } else {
                    uint32_t default_status = 0;  /* 业务模式 */
                    memcpy(run_resp, &default_status, 4);
                }

                add_ie(&payload, &offset, IE_TYPE_RUN_STATUS_RESP, run_resp, sizeof(run_resp));
                break;
            }

            case IE_TYPE_CPRI_MODE_QUERY: {
                /* CPRI口工作模式查询 */
                uint8_t cpri_resp[4];

                if (has_fpga_status) {
                    uint32_t cpri_mode = fpga_status.data.cpri_work_mode;
                    memcpy(cpri_resp, &cpri_mode, 4);
                    //LOG_DEBUG("Added CPRI mode response: mode=%u", cpri_mode);
                } else {
                    uint32_t default_mode = 0;  /* 普通模式 */
                    memcpy(cpri_resp, &default_mode, 4);
                }

                add_ie(&payload, &offset, IE_TYPE_CPRI_MODE_RESP, cpri_resp, sizeof(cpri_resp));
                break;
            }

            case IE_TYPE_CALIB_RESULT_QUERY: {
                /* 校准结果查询 */
                uint8_t calib_resp[1];
                calib_resp[0] = 0;  /* 0=成功 */

                add_ie(&payload, &offset, IE_TYPE_CALIB_RESULT_RESP, calib_resp, sizeof(calib_resp));
               // LOG_DEBUG("Added calibration result response: result=%u", calib_resp[0]);
                break;
            }

            default:
                LOG_DEBUG("Unknown query IE type: %u", ie_type);
                break;
        }

        req_offset += ie_len;
    }

    response->payload = payload;
    response->payload_len = offset;

    LOG_INFO("Created status query response: %u IEs, payload_len=%u",
             offset > 0 ? offset / 10 : 0, offset);
    return SUCCESS;
}

int status_query_handle_request(const cpri_message_t *msg)
{
    if (!msg) {
        return ERROR_INVALID_PARAM;
    }

    LOG_INFO("Handling status query request");
    return SUCCESS;
}
