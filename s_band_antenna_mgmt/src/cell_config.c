#include "cell_config.h"
#include "fpga_handler.h"
#include "logger.h"
#include "tcp_client.h"
#include <string.h>
#include <stdlib.h>

/* 全局小区配置管理器 */
static cell_config_manager_t g_cell_mgr;

/* 全局TCP客户端（定义在 main.c） */
extern tcp_client_t g_tcp_client;

/* ==================== 频点配置异步执行 ====================
 * wait_for_antenna_mode_cycle() 最长阻塞 5 秒（100ms 轮询）。它原本跑在 TCP
 * 接收线程上，阻塞期间不读 socket → BBU 心跳积压在内核缓冲 → 误判 BBU 失联
 * （现场 25 次心跳超时中 8 次归因于此；实测：改造前接收线程被堵满 5 秒，
 * 期间到达的后续请求全部排队，改造后每条请求都在到达时刻被立即受理）。
 *
 * 这里把整个 MsgID 195 的处理搬到独立 worker 线程：
 *   接收线程只做一次深拷贝入队后立即返回；
 *   worker 在阻塞结束后构造并发出响应。
 * 对 BBU 而言应答内容与时机均不变，只是不再占用接收线程。
 * ======================================================== */
#define CELL_CFG_JOB_QUEUE_LEN 8
#define CELL_CFG_RESP_BUF_SIZE 4096

typedef struct {
    uint8_t  paau_id;
    uint8_t  bbu_id;
    uint8_t  port_num;
    uint32_t serial_num;
    uint32_t payload_len;
    uint8_t *payload;   /* 深拷贝：dispatch 返回后原 payload 立刻被还给内存池 */
} cell_cfg_job_t;

/* g_cell_mgr.mutex 是否处于"已初始化且未销毁"状态。
 * 必要性：init 在 worker 创建失败时会销毁该 mutex 再返回失败，
 * 而 main.c 的清理仍会无条件调用 cell_config_destroy() → 二次 destroy（UB）。 */
static bool g_cell_mgr_mutex_ready = false;

static cell_cfg_job_t  *g_job_queue[CELL_CFG_JOB_QUEUE_LEN];
static int              g_job_head = 0;
static int              g_job_tail = 0;
static int              g_job_count = 0;

/* 前向声明：cell_config_send_busy_response 在文件前部就要用 add_ie，
 * 而它的定义在后面的响应构造区 */
static int add_ie(uint8_t **payload, uint32_t *offset, uint16_t ie_type,
                  const uint8_t *ie_data, uint16_t ie_data_len);
static pthread_mutex_t  g_job_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_job_cond  = PTHREAD_COND_INITIALIZER;
static pthread_t        g_job_thread;
static bool             g_job_thread_valid = false;
static bool             g_job_thread_stop  = false;

static void cell_cfg_job_free(cell_cfg_job_t *job)
{
    if (job) {
        free(job->payload);
        free(job);
    }
}

/* worker 线程：取作业 → 执行会阻塞的处理 → 发出响应 */
static void *cell_cfg_worker(void *arg)
{
    (void)arg;

    for (;;) {
        pthread_mutex_lock(&g_job_mutex);
        while (g_job_count == 0 && !g_job_thread_stop) {
            pthread_cond_wait(&g_job_cond, &g_job_mutex);
        }
        if (g_job_count == 0 && g_job_thread_stop) {
            pthread_mutex_unlock(&g_job_mutex);
            break;
        }

        cell_cfg_job_t *job = g_job_queue[g_job_head];
        g_job_queue[g_job_head] = NULL;
        g_job_head = (g_job_head + 1) % CELL_CFG_JOB_QUEUE_LEN;
        g_job_count--;
        pthread_mutex_unlock(&g_job_mutex);

        /* 用深拷贝的数据重建只读请求视图，复用原有的响应构造逻辑 */
        cpri_message_t request;
        memset(&request, 0, sizeof(request));
        request.header.msg_id     = MSG_NR_CELL_CONFIG;
        request.header.paau_id    = job->paau_id;
        request.header.bbu_id     = job->bbu_id;
        request.header.port_num   = job->port_num;
        request.header.serial_num = job->serial_num;
        request.payload           = job->payload;
        request.payload_len       = job->payload_len;

        cpri_message_t response;
        if (cell_config_create_response(&response, &request) == SUCCESS) {
            uint8_t buffer[CELL_CFG_RESP_BUF_SIZE];
            int len = cpri_encode_message(&response, buffer, sizeof(buffer));
            if (len > 0) {
                tcp_client_send(&g_tcp_client, buffer, len);
                LOG_INFO("Sent cell config response (serial_num=%u)", response.header.serial_num);
            } else {
                LOG_ERROR("Failed to encode cell config response (serial=%u)", job->serial_num);
            }
            cpri_free_message(&response);
        } else {
            LOG_ERROR("Failed to create cell config response (serial=%u)", job->serial_num);
        }

        cell_cfg_job_free(job);
    }

    LOG_INFO("Cell config worker thread exited");
    return NULL;
}

/* 队列已满、无法入队时，给 BBU 回一个明确的失败响应。
 *
 * 原行为是直接丢包并只打日志 —— BBU 一直在等一个永不到来的
 * MSG_NR_CELL_CONFIG_RSP。worker 单次最长会阻塞 5 秒（天线模式校验），
 * 队列只有 8 格，BBU 重试风暴下很容易撞满。这里按请求里的 IE 逐个回失败，
 * 不解析也不应用任何配置（本地 cell_id/beam_id 填 0 占位），
 * 只把"本设备当前处理不了"如实告诉对端，让它可以稍后重试。 */
static void cell_config_send_busy_response(const cpri_message_t *msg)
{
    if (!msg || !msg->payload) {
        return;
    }

    uint8_t *payload = NULL;
    uint32_t offset = 0;
    uint32_t req_offset = 0;

    while (req_offset + 4 <= msg->payload_len) {
        uint16_t ie_type, ie_len;
        memcpy(&ie_type, msg->payload + req_offset, 2);
        memcpy(&ie_len, msg->payload + req_offset + 2, 2);

        if (ie_len < 4 || req_offset + ie_len > msg->payload_len) {
            break;
        }

        if (ie_type == IE_TYPE_CELL_CONFIG) {
            uint8_t resp[8];
            memset(resp, 0, sizeof(resp));
            memcpy(resp + 4, &(uint32_t){ CONFIG_RESULT_FAILURE }, 4);
            add_ie(&payload, &offset, IE_TYPE_CELL_CONFIG_RESP, resp, sizeof(resp));
        } else if (ie_type == IE_TYPE_FREQ_CONFIG) {
            uint8_t resp[9];
            memset(resp, 0, sizeof(resp));
            memcpy(resp + 5, &(uint32_t){ CONFIG_RESULT_FAILURE }, 4);
            add_ie(&payload, &offset, IE_TYPE_FREQ_CONFIG_RESP, resp, sizeof(resp));
        }

        req_offset += ie_len;
    }

    if (!payload || offset == 0) {
        free(payload);
        return;
    }

    cpri_message_t response;
    memset(&response, 0, sizeof(response));
    response.header.msg_id     = MSG_NR_CELL_CONFIG_RSP;
    response.header.paau_id    = msg->header.paau_id;
    response.header.bbu_id     = msg->header.bbu_id;
    response.header.port_num   = msg->header.port_num;
    response.header.serial_num = msg->header.serial_num;
    response.payload           = payload;
    response.payload_len       = offset;

    uint8_t buffer[CELL_CFG_RESP_BUF_SIZE];
    int len = cpri_encode_message(&response, buffer, sizeof(buffer));
    if (len > 0) {
        tcp_client_send(&g_tcp_client, buffer, len);
        LOG_WARN("Sent cell config BUSY/failure response (serial=%u, payload_len=%u)",
                 response.header.serial_num, offset);
    } else {
        LOG_ERROR("Failed to encode busy response (serial=%u)", msg->header.serial_num);
    }
    free(payload);
}

/* 接收线程调用：深拷贝请求并入队，立即返回（不阻塞） */
int cell_config_submit_request(const cpri_message_t *msg)
{
    if (!msg) {
        return ERROR_INVALID_PARAM;
    }

    cell_cfg_job_t *job = (cell_cfg_job_t *)calloc(1, sizeof(cell_cfg_job_t));
    if (!job) {
        LOG_ERROR("Failed to alloc cell cfg job");
        return ERROR_GENERAL;
    }

    job->paau_id     = msg->header.paau_id;
    job->bbu_id      = msg->header.bbu_id;
    job->port_num    = msg->header.port_num;
    job->serial_num  = msg->header.serial_num;
    job->payload_len = msg->payload_len;

    if (msg->payload_len > 0 && msg->payload) {
        job->payload = (uint8_t *)malloc(msg->payload_len);
        if (!job->payload) {
            LOG_ERROR("Failed to alloc job payload (%u bytes)", msg->payload_len);
            free(job);
            return ERROR_GENERAL;
        }
        memcpy(job->payload, msg->payload, msg->payload_len);
    }

    pthread_mutex_lock(&g_job_mutex);
    if (g_job_count >= CELL_CFG_JOB_QUEUE_LEN) {
        pthread_mutex_unlock(&g_job_mutex);
        LOG_ERROR("Cell cfg job queue full, request dropped (serial=%u)", job->serial_num);
        cell_cfg_job_free(job);
        /* 必须回一个失败响应：否则 BBU 会一直等一个不会到来的响应。
         * 本函数跑在 TCP 接收线程上，但这里只做一次组帧+send，不阻塞
         * （不能在此队列已满的时刻再去排队或等待）。 */
        cell_config_send_busy_response(msg);
        return ERROR_GENERAL;
    }
    g_job_queue[g_job_tail] = job;
    g_job_tail = (g_job_tail + 1) % CELL_CFG_JOB_QUEUE_LEN;
    g_job_count++;
    pthread_cond_signal(&g_job_cond);
    pthread_mutex_unlock(&g_job_mutex);

    LOG_INFO("Cell config request queued (serial=%u, payload_len=%u)",
             job->serial_num, job->payload_len);
    return SUCCESS;
}

/**
 * 检查是否有任何已下发到天线并生效的频点
 * 返回：true = 有，false = 没有
 *
 * 注意：不能用 cells[].freq_count 判断——它把因天线模式不对而仅本地留存、
 * 从未下发的频点也计入了。若据此开启发射，会出现天线侧已无有效频点、
 * 发射却仍被这些"僵尸记录"支撑着不关的情况。这里只认 sent_to_antenna。
 */
static bool has_any_freq(void)
{
    for (int i = 0; i < MAX_CELLS; i++) {
        if (!g_cell_mgr.cells[i].active) {
            continue;
        }
        for (int j = 0; j < MAX_FREQS_PER_CELL; j++) {
            if (g_cell_mgr.freqs[i][j].active &&
                g_cell_mgr.freqs[i][j].sent_to_antenna) {
                return true;
            }
        }
    }
    return false;
}

/* ==================== 发射控制 ====================
 * 是否开发射由两个条件共同决定：
 *   1) 天线侧存在已下发并生效的频点 (has_any_freq)；
 *   2) 与基带(BBU)处于可通信状态 (g_bbu_link_ok)。
 * 与基带无法通信时禁止开发射，并在判定失联的瞬间下发一次关闭发射。
 * ================================================= */

/* 与基带是否处于可通信状态。false = 已判定"与基带无法通信"，此期间禁止开发射。
 * 由 cell_config_notify_bbu_link() 在链路事件处更新（TCP 接收线程 / 主线程），
 * 由 worker 线程在 update_tx_control() 里读取，故用独立互斥锁保护。
 * 锁序固定为 g_cell_mgr.mutex → g_tx_guard_mutex：update_tx_control() 是在持有
 * g_cell_mgr.mutex 的情况下取本锁的，而反向取锁的路径不存在，不会死锁。
 * 初值 false：上电后尚未与基带建立通道，不认为可通信。
 * 本锁在其生命周期内不销毁：cell_config_destroy() 之后 TCP 接收线程仍可能
 * 回调 cell_config_notify_bbu_link()（main.c 先销毁 cell_config 再停 TCP），
 * 销毁本锁会让那条路径锁到已销毁的互斥量。 */
static pthread_mutex_t g_tx_guard_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool            g_bbu_link_ok = false;

/* 本地记录的发射开关状态。FPGA 的 0x0B(发射控制) 无应答，"当前是否在发射"
 * 只能以本地下发成功为准；用于把"基带失联 → 关发射"做成 发→关 跳变去重，
 * 避免链路抖动时反复下发同一条指令。上电按 FPGA 默认关发射处理。 */
static bool            g_tx_on = false;

/* 下发发射控制并记录状态。仅在下发成功时更新记录：
 * 若本次下发失败，记录仍停留在"发射中"，于是后续任何一次链路事件都会再次
 * 尝试补发关闭指令。注意这【不是】本次失联期间的周期重发（按策略只在
 * 发→关跳变时发一次），UART 写失败会打 ERROR 供现场定位。 */
static void apply_tx_control(bool on, const char *reason)
{
    int ret = fpga_send_tx_control(on ? 1 : 0);
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to %s TX (%s)", on ? "enable" : "disable",
                  reason ? reason : "unknown");
        return;
    }

    pthread_mutex_lock(&g_tx_guard_mutex);
    g_tx_on = on;
    pthread_mutex_unlock(&g_tx_guard_mutex);

    LOG_INFO("TX %s (%s)", on ? "enabled" : "disabled",
             reason ? reason : "unknown");
}

/**
 * 根据全局频点状态与基带链路状态控制发射
 */
static void update_tx_control(void)
{
    bool link_ok;

    pthread_mutex_lock(&g_tx_guard_mutex);
    link_ok = g_bbu_link_ok;
    pthread_mutex_unlock(&g_tx_guard_mutex);

    /* 与基带无法通信：合上"开射"闸门。关闭指令已由 cell_config_notify_bbu_link()
     * 在判定失联时下发，这里直接返回；等基带重新建立通道并重新下发频点配置后，
     * 本函数才会再次放开。这一道闸门也挡掉了链路中断前入队、中断后才跑完的
     * 频点配置作业（worker 里最长会等 5 秒天线模式校验），否则它会把发射又打开。 */
    if (!link_ok) {
        LOG_WARN("BBU link down, TX enable suppressed (has_freq=%d)", has_any_freq());
        return;
    }

    if (has_any_freq()) {
        /* 有频点，开启发射 */
        apply_tx_control(true, "at least one cell has freqs");
    } else {
        /* 没有频点，关闭发射 */
        apply_tx_control(false, "no freqs in any cell");
    }
}

/**
 * 通知基带(BBU)链路状态 —— "与基带无法通信时关闭发射"的落点
 *
 * up=false：判定与基带无法通信。若本地记录发射为开启，向天线下发一次关闭发射，
 *           并禁止后续开发射，直到基带重新建立通道。
 * up=true ：基带通道已重新建立，放开"允许开发射"的闸门。此处【不】直接开射，
 *           实际开射仍由基带随后的频点配置经 update_tx_control() 触发。
 *
 * 可重复调用：只在状态跳变时产生动作与日志，不会因为每秒轮询而重复下发。
 */
void cell_config_notify_bbu_link(bool up, const char *reason)
{
    const char *why = reason ? reason : "unknown";
    bool was_ok;
    bool need_off = false;

    pthread_mutex_lock(&g_tx_guard_mutex);
    was_ok = g_bbu_link_ok;

    if (up) {
        g_bbu_link_ok = true;
    } else {
        g_bbu_link_ok = false;
        /* 只在本地认为"正在发射"时才下发关闭，避免链路抖动时反复发同一条指令 */
        need_off = g_tx_on;
    }
    pthread_mutex_unlock(&g_tx_guard_mutex);

    if (up) {
        if (!was_ok) {
            LOG_INFO("BBU link available again (%s), TX enable re-armed", why);
        }
        return;
    }

    if (!was_ok && !need_off) {
        /* 链路本就不通、发射本就没开：无动作，也不重复打日志 */
        return;
    }

    LOG_WARN("BBU link lost (%s)", why);

    if (need_off) {
        apply_tx_control(false, "BBU link lost");
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
    g_cell_mgr_mutex_ready = true;

    /* 启动异步 worker：把 5 秒模式校验移出 TCP 接收线程 */
    g_job_head = 0;
    g_job_tail = 0;
    g_job_count = 0;
    g_job_thread_stop = false;
    if (pthread_create(&g_job_thread, NULL, cell_cfg_worker, NULL) != 0) {
        LOG_ERROR("Failed to create cell config worker thread");
        pthread_mutex_destroy(&g_cell_mgr.mutex);
        g_cell_mgr_mutex_ready = false;   /* 已销毁，cleanup 不得再销毁一次 */
        return ERROR_GENERAL;
    }
    g_job_thread_valid = true;

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
                    g_cell_mgr.freqs[cell_idx][i].sent_to_antenna = false;
                }
                g_cell_mgr.cells[cell_idx].freq_count = 0;  /* 重置频点计数 */
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
 * 发送频点配置后，天线会从某个模式切到其他模式，然后再切回原模式
 *
 * 支持两种模式切换场景：
 *   1. 业务模式 -> 其他模式 -> 业务模式
 *   2. 频谱监测模式 -> 其他模式 -> 频谱监测模式
 *
 * 返回值：
 *   SUCCESS - 检测到模式切换周期完成
 *   ERROR_TIMEOUT - 超时未检测到预期的模式切换
 *   ERROR_GENERAL - 其他错误
 */
static int wait_for_antenna_mode_cycle(void)
{
    const int MAX_WAIT_MS = 5000;       /* 最大等待时间：5秒 */
    const int POLL_INTERVAL_MS = 100;   /* 轮询间隔：100ms */
    int elapsed_ms = 0;

    bool mode_changed = false;
    uint32_t initial_mode;

    /* 获取初始工作模式 */
    fpga_status_frame_t fpga_status;
    if (fpga_handler_get_status(&fpga_status) != SUCCESS) {
        LOG_ERROR("Failed to get initial antenna mode");
        return ERROR_GENERAL;
    }
    initial_mode = fpga_status.data.phased_array_work_mode;

    /* 只对业务模式和频谱监测模式进行模式切换验证 */
    if (initial_mode != PHASED_ARRAY_MODE_BUSINESS &&
        initial_mode != PHASED_ARRAY_MODE_SPECTRUM) {
        LOG_INFO("Antenna in mode %u, skip mode cycle verification", initial_mode);
        return SUCCESS;
    }

    LOG_INFO("Waiting for antenna work mode to cycle (mode %u -> other -> mode %u)...",
             initial_mode, initial_mode);

    /* 第一阶段：等待模式从初始模式切换到其他模式 */
    while (elapsed_ms < MAX_WAIT_MS && !mode_changed) {
        if (fpga_handler_get_status(&fpga_status) == SUCCESS) {
            uint32_t current_mode = fpga_status.data.phased_array_work_mode;

            if (current_mode != initial_mode) {
                LOG_INFO("Antenna mode changed from %u to %u", initial_mode, current_mode);
                mode_changed = true;
                break;
            }
        }

        usleep(POLL_INTERVAL_MS * 1000);
        elapsed_ms += POLL_INTERVAL_MS;
    }

    if (!mode_changed) {
        LOG_WARN("Antenna mode did not change from mode %u within %d ms",
                 initial_mode, MAX_WAIT_MS);
        /* 可能天线已经配置好了，不算错误 */
        return SUCCESS;
    }

    /* 第二阶段：等待模式切回初始模式 */
    bool mode_back_to_initial = false;
    while (elapsed_ms < MAX_WAIT_MS) {
        if (fpga_handler_get_status(&fpga_status) == SUCCESS) {
            uint32_t current_mode = fpga_status.data.phased_array_work_mode;

            if (current_mode == initial_mode) {
                LOG_INFO("Antenna mode returned to mode %u (cycle completed)", initial_mode);
                mode_back_to_initial = true;
                break;
            }
        }

        usleep(POLL_INTERVAL_MS * 1000);
        elapsed_ms += POLL_INTERVAL_MS;
    }

    if (!mode_back_to_initial) {
        LOG_ERROR("Antenna mode did not return to mode %u within %d ms",
                  initial_mode, MAX_WAIT_MS);
        return ERROR_TIMEOUT;
    }

    LOG_INFO("Antenna work mode cycle verification completed successfully (took %d ms)", elapsed_ms);
    return SUCCESS;
}

/**
 * 把频点配置写入本地管理结构
 * sent_to_antenna：本次写入的参数是否已经真正下发到天线并验证通过。
 *   true  - 记录可用于后续查重（认为天线侧已是该配置）
 *   false - 仅本地留存（天线模式不对、下发失败等），查重时必须忽略，
 *           否则 BBU 重发同一条频点配置会被误判为"已配好"而跳过下发
 */
static void store_freq_info(freq_info_t *slot, const freq_config_ie_t *freq_cfg, bool sent_to_antenna)
{
    slot->active = true;
    slot->sent_to_antenna = sent_to_antenna;
    slot->local_cell_id = freq_cfg->local_cell_id;
    slot->beam_id = freq_cfg->beam_id;
    slot->dl_center_freq = freq_cfg->dl_center_freq;
    slot->ul_center_freq = freq_cfg->ul_center_freq;
    slot->beam_bandwidth = freq_cfg->beam_bandwidth;
    slot->ul_dl_config = freq_cfg->ul_dl_config;
    slot->special_subframe = freq_cfg->special_subframe;
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
            /* 步骤1：首先检查天线工作模式（前置条件） */
            fpga_status_frame_t fpga_status;
            uint32_t current_mode;
            bool mode_check_passed = false;

            if (fpga_handler_get_status(&fpga_status) == SUCCESS) {
                current_mode = fpga_status.data.phased_array_work_mode;

                if (current_mode == PHASED_ARRAY_MODE_BUSINESS ||
                    current_mode == PHASED_ARRAY_MODE_SPECTRUM) {
                    mode_check_passed = true;
                    LOG_DEBUG("Antenna mode check passed: mode=%u", current_mode);
                } else {
                    LOG_INFO("Antenna in mode %u (not business/spectrum), reject freq config",
                             current_mode);
                }
            } else {
                LOG_ERROR("Failed to get antenna mode, reject freq config");
            }

            /* 如果工作模式不正确，直接返回失败，统一处理所有频点配置 */
            if (!mode_check_passed) {
                /* 仍然需要保存频点配置到本地管理结构 */
                int freq_idx = -1;

                /* 查找是否已存在该 beam_id */
                for (int i = 0; i < MAX_FREQS_PER_CELL; i++) {
                    if (g_cell_mgr.freqs[cell_idx][i].active &&
                        g_cell_mgr.freqs[cell_idx][i].beam_id == freq_cfg->beam_id) {
                        freq_idx = i;
                        break;
                    }
                }

                if (freq_idx < 0) {
                    /* 查找空闲位置 */
                    for (int i = 0; i < MAX_FREQS_PER_CELL; i++) {
                        if (!g_cell_mgr.freqs[cell_idx][i].active) {
                            freq_idx = i;
                            g_cell_mgr.cells[cell_idx].freq_count++;
                            break;
                        }
                    }
                }

                /* 保存频点配置（即使不发送到天线） */
                if (freq_idx >= 0) {
                    /* 标记为未下发：天线模式不对，参数只存在本地，
                     * 后续 BBU 重发时必须重新下发，不能被查重跳过 */
                    store_freq_info(&g_cell_mgr.freqs[cell_idx][freq_idx], freq_cfg, false);

                    LOG_INFO("Freq saved locally (not sent): cell=%u, beam=%u, dl=%u kHz, ul=%u kHz, bw=%u",
                             freq_cfg->local_cell_id, freq_cfg->beam_id,
                             freq_cfg->dl_center_freq, freq_cfg->ul_center_freq, freq_cfg->beam_bandwidth);
                } else {
                    LOG_ERROR("No free freq slot for cell %u (mode check failed)",
                              freq_cfg->local_cell_id);
                }

                /* 统一返回失败 */
                *result = CONFIG_RESULT_FAILURE;
                break;  /* 提前退出 case */
            }

            /* 步骤2：工作模式正确，检查是否需要发送到天线 */
            bool need_send_to_antenna = true;

            /* 本小区范围内查重：查找是否已有频点使用相同的频率配置。
             * 只把 sent_to_antenna 为 true 的记录算作重复——
             * 之前因模式不对而仅本地留存、从未下发的记录，其参数并不代表
             * 天线侧的真实状态，若计入查重会导致本次重发被误判为"已配好"
             * 而跳过下发，最终 BBU 认为成功、天线却没有该频点。 */
            for (int i = 0; i < MAX_FREQS_PER_CELL; i++) {
                if (g_cell_mgr.freqs[cell_idx][i].active &&
                    g_cell_mgr.freqs[cell_idx][i].sent_to_antenna &&
                    g_cell_mgr.freqs[cell_idx][i].dl_center_freq == freq_cfg->dl_center_freq &&
                    g_cell_mgr.freqs[cell_idx][i].ul_center_freq == freq_cfg->ul_center_freq &&
                    g_cell_mgr.freqs[cell_idx][i].beam_bandwidth == freq_cfg->beam_bandwidth) {
                    /* 找到相同的频率配置且已下发到天线，无需重复发送 */
                    need_send_to_antenna = false;
                    LOG_INFO("Freq beam=%u has same config as already-sent beam=%u (DL=%u, UL=%u, BW=%u), skip antenna command",
                             freq_cfg->beam_id,
                             g_cell_mgr.freqs[cell_idx][i].beam_id,
                             freq_cfg->dl_center_freq,
                             freq_cfg->ul_center_freq,
                             freq_cfg->beam_bandwidth);
                    break;
                }
            }

            /* 步骤3：查找或分配频点槽位 */
            int freq_idx = -1;

            /* 查找是否已存在该 beam_id */
            for (int i = 0; i < MAX_FREQS_PER_CELL; i++) {
                if (g_cell_mgr.freqs[cell_idx][i].active &&
                    g_cell_mgr.freqs[cell_idx][i].beam_id == freq_cfg->beam_id) {
                    freq_idx = i;
                    break;
                }
            }

            if (freq_idx >= 0) {
                /* 频点beam_id已存在，更新参数 */
                if (need_send_to_antenna) {
                    LOG_INFO("Freq beam=%u already exists, updating with new params", freq_cfg->beam_id);
                }
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

            /* 步骤4：保存频点配置到本地管理结构
             * 此处统一按"未下发"记录，只有下面确认天线侧生效后才置
             * sent_to_antenna，避免下发失败或模式验证超时时留下脏记录 */
            store_freq_info(&g_cell_mgr.freqs[cell_idx][freq_idx], freq_cfg, false);

            LOG_INFO("Freq configured: cell=%u, beam=%u, dl=%u kHz, ul=%u kHz, bw=%u",
                     freq_cfg->local_cell_id, freq_cfg->beam_id,
                     freq_cfg->dl_center_freq, freq_cfg->ul_center_freq, freq_cfg->beam_bandwidth);

            /* 步骤5：如果需要，发送到天线并等待模式切换 */
            if (need_send_to_antenna) {
                /* 发送频点配置到天线 */
                int ret = fpga_send_freq_band(freq_cfg->dl_center_freq,
                                              freq_cfg->ul_center_freq,
                                              freq_cfg->beam_bandwidth);
                if (ret != SUCCESS) {
                    LOG_ERROR("Failed to send freq/band to FPGA");
                    *result = CONFIG_RESULT_FAILURE;
                    /* sent_to_antenna 保持 false，BBU 重发时会重新下发 */
                } else {
                    LOG_INFO("Sent freq/band to antenna: DL=%u kHz, UL=%u kHz, BW=%u (mode=%u)",
                             freq_cfg->dl_center_freq, freq_cfg->ul_center_freq,
                             freq_cfg->beam_bandwidth, current_mode);

                    /* 释放锁以允许状态更新 */
                    pthread_mutex_unlock(&g_cell_mgr.mutex);

                    /* 等待并验证天线工作模式切换 */
                    int mode_check_ret = wait_for_antenna_mode_cycle();

                    /* 重新加锁以保持函数退出时的锁状态一致 */
                    pthread_mutex_lock(&g_cell_mgr.mutex);

                    /* 等待期间锁被释放，小区和频点槽位可能已被其他线程
                     * 删除或复用，必须重新定位后再置位，否则会误标到别的频点 */
                    int cur_cell_idx = -1;
                    for (int i = 0; i < MAX_CELLS; i++) {
                        if (g_cell_mgr.cells[i].active &&
                            g_cell_mgr.cells[i].local_cell_id == freq_cfg->local_cell_id) {
                            cur_cell_idx = i;
                            break;
                        }
                    }

                    int cur_freq_idx = -1;
                    if (cur_cell_idx >= 0) {
                        for (int i = 0; i < MAX_FREQS_PER_CELL; i++) {
                            freq_info_t *f = &g_cell_mgr.freqs[cur_cell_idx][i];
                            if (f->active && f->beam_id == freq_cfg->beam_id &&
                                f->dl_center_freq == freq_cfg->dl_center_freq &&
                                f->ul_center_freq == freq_cfg->ul_center_freq &&
                                f->beam_bandwidth == freq_cfg->beam_bandwidth) {
                                cur_freq_idx = i;
                                break;
                            }
                        }
                    }

                    if (cur_freq_idx < 0) {
                        /* 等待期间配置已被改动，本次结果作废，交由 BBU 重发 */
                        LOG_WARN("Cell %u beam %u changed during mode wait, discard this result",
                                 freq_cfg->local_cell_id, freq_cfg->beam_id);
                        *result = CONFIG_RESULT_FAILURE;
                    } else if (mode_check_ret != SUCCESS) {
                        LOG_ERROR("Antenna work mode cycle verification failed");
                        *result = CONFIG_RESULT_FAILURE;
                        /* sent_to_antenna 保持 false，BBU 重发时会重新下发 */
                    } else {
                        /* 天线侧已确认生效，标记该记录可用于后续查重 */
                        g_cell_mgr.freqs[cur_cell_idx][cur_freq_idx].sent_to_antenna = true;
                        *result = CONFIG_RESULT_SUCCESS;

                        /* 检查全局频点状态并更新发射控制 */
                        update_tx_control();
                    }
                }
            } else {
                /* 命中同小区内一条已下发的同参数记录。天线侧的频段配置是全局的
                 * （fpga_send_freq_band 不区分 beam_id），说明天线当前就是这组参数，
                 * 因此本条记录同样标记为已下发 */
                g_cell_mgr.freqs[cell_idx][freq_idx].sent_to_antenna = true;
                *result = CONFIG_RESULT_SUCCESS;

                /* 配置相同且工作模式正确，无需等待模式切换，直接更新发射控制 */
                update_tx_control();
            }

            break;
        }

        case FREQ_CFG_DELETE: {
            /* 删除频点 */
            if (freq_idx >= 0) {
                g_cell_mgr.freqs[cell_idx][freq_idx].active = false;
                g_cell_mgr.freqs[cell_idx][freq_idx].sent_to_antenna = false;
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

        /* IE长度包含IE头本身，不能小于IE头长度(4字节)；否则 offset 不推进将导致死循环 */
        if (ie_len < 4 || req_offset + ie_len > request->payload_len) {
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
    /* 必须先停 worker 并 join，否则它可能在 mutex 销毁后仍访问全局状态与 g_tcp_client。
     * join 最长等待一次模式校验（5 秒），有界。 */
    if (g_job_thread_valid) {
        pthread_mutex_lock(&g_job_mutex);
        g_job_thread_stop = true;
        pthread_cond_signal(&g_job_cond);
        pthread_mutex_unlock(&g_job_mutex);

        pthread_join(g_job_thread, NULL);
        g_job_thread = 0;
        g_job_thread_valid = false;

        /* 释放队列中尚未处理的作业 */
        for (int i = 0; i < CELL_CFG_JOB_QUEUE_LEN; i++) {
            cell_cfg_job_free(g_job_queue[i]);
            g_job_queue[i] = NULL;
        }
        g_job_count = 0;
    }

    if (g_cell_mgr_mutex_ready) {
        pthread_mutex_destroy(&g_cell_mgr.mutex);
        g_cell_mgr_mutex_ready = false;
    }
    LOG_INFO("Cell config manager destroyed");
}
