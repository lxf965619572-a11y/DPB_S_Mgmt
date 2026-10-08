#include "telemetry_log.h"
#include "uart_client.h"
#include "fpga_protocol.h"
#include "config.h"
#include "logger.h"
#include "shell_util.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <libgen.h>

/* UART 客户端（定义在 main.c）。轮询线程经它下发命令。 */
extern uart_client_t g_uart_client;

/* ==================== 环形队列 ====================
 * 接收线程 → 写盘线程的定长内存缓冲。两点是刻意的：
 *   1) 不用 malloc：分配失败会在接收路径上凭空多出一种失败模式，而槽位本来就是定长的；
 *   2) 必须无 I/O：接收线程同时还承载"0xEB90 状态帧 → fpga_handler → 告警判据"这条路径。
 *      一旦它在磁盘上阻塞（eMMC 擦除风暴、分区写满后的元数据等待），所有 FPGA 遥测都会
 *      停更，告警会基于过期遥测做判断。丢一帧遥测日志的代价远小于阻塞接收线程。
 * 32 槽 ≈ 10 分钟积压，足以吸收一次上传快照的复制过程。 */
#define TEL_QUEUE_SLOTS     32
#define TEL_SLOT_MAX_FRAME  160     /* 6+146=152，留一点余量 */

typedef struct {
    uint16_t len;
    uint8_t  data[TEL_SLOT_MAX_FRAME];
} tel_slot_t;

static tel_slot_t g_tel_ring[TEL_QUEUE_SLOTS];
static uint32_t   g_tel_head = 0;
static uint32_t   g_tel_tail = 0;
static uint32_t   g_tel_count = 0;
static uint64_t   g_tel_dropped = 0;        /* 队列满丢弃累计 */
static bool       g_tel_stopping = false;   /* 置位后回调不再入队、两个线程退出 */

/* 队列锁：只在入队/出队的瞬间持有，绝不跨 I/O。这是"接收线程不会被写盘或快照
 * 阻塞"得以成立的前提。 */
static pthread_mutex_t g_tel_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 文件锁：保护 g_tel_fp、轮转与快照复制。需要它是因为文件有第二个访问者——
 * 上传线程的快照复制必须与轮转互斥。
 * 本模块只存在 file_mutex → log_mutex 一个方向的锁序（持文件锁时可能打日志）；
 * logger_log 的临界区只碰它自己的静态量、从不回调本模块，故不成环。 */
static pthread_mutex_t g_tel_file_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 两个条件变量刻意不共用：pthread_cond_signal 只唤醒一个等待者，若轮询线程与
 * 写盘线程共用一个 condvar，"有数据了"的信号可能被轮询线程吃掉，写盘线程要等到
 * 下一帧才醒（丢唤醒，队列白积压）。分开则各自的谓词互不干扰。
 * 两者都不能用静态初始化器：需要把时钟设成 CLOCK_MONOTONIC（见 init）。 */
static pthread_cond_t g_tel_data_cond;   /* 生产者 → 写盘线程 */
static pthread_cond_t g_tel_timer_cond;  /* destroy → 轮询线程 */

static pthread_t g_tel_poll_tid;
static pthread_t g_tel_writer_tid;
static bool      g_tel_poll_valid = false;
static bool      g_tel_writer_valid = false;
static bool      g_tel_cond_valid = false;
static bool      g_tel_inited = false;

static FILE  *g_tel_fp = NULL;
static char   g_tel_path[256] = TELEMETRY_LOG_DEFAULT_PATH;
static size_t g_tel_bytes = 0;          /* 当前文件已写字节数（本进程是唯一写入者，故记账而非每次 stat） */
static size_t g_tel_max_size = 4 * 1024 * 1024;
static int    g_tel_max_backups = 5;
static uint32_t g_tel_poll_sec = 60;
static uint32_t g_tel_poll_gap_ms = 100;

/* 取单调时钟绝对时刻。轮询周期与线程等待都不能用 CLOCK_REALTIME：本机会给 FPGA
 * 下发系统时间、系统侧也可能做 NTP 校正，墙钟被回拨时"等 60 秒"会变成几十分钟
 * （现场表现为"遥测日志突然长时间不更新"）。 */
static void tel_now(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

/* ==================== 轮转 ====================
 * unlink 最旧备份 → 倒序 rename → rename 当前 → 重新打开。
 * 与 logger.c 的轮转同策略，但有三处刻意不同，直接照抄会出事：
 *   1) 绝不往文件里写任何文本。logger 的轮转会在文件里写 "Starting log rotation..."
 *      之类的横幅，而本文件是纯二进制帧流，混入一行文本会让对端按帧切分时整段错位。
 *      轮转信息只走 LOG_INFO 进文本日志。
 *   2) 轮转前 fsync 一次（logger 完全不 fsync）。必须在 rename 之前：否则元数据先落盘、
 *      数据还在页缓存，掉电后 .1 里是空洞。
 *   3) 大小不看 stat 而用 g_tel_bytes 记账 —— 本进程是唯一写入者。
 * 调用前必须持有 g_tel_file_mutex。 */
static void tel_rotate_locked(void)
{
    char old_path[300];
    char new_path[300];
    int i;

    if (g_tel_fp) {
        if (fflush(g_tel_fp) != 0) {
            LOG_WARN("Telemetry log flush before rotate failed: %s", strerror(errno));
        }
        if (fsync(fileno(g_tel_fp)) != 0) {
            LOG_WARN("Telemetry log fsync failed: %s", strerror(errno));
        }
        fclose(g_tel_fp);
        g_tel_fp = NULL;
    }

    if (g_tel_max_backups > 0) {
        snprintf(old_path, sizeof(old_path), "%s.%d", g_tel_path, g_tel_max_backups);
        unlink(old_path);   /* 最旧的删不掉不影响正确性 */
    }

    for (i = g_tel_max_backups - 1; i >= 1; i--) {
        snprintf(old_path, sizeof(old_path), "%s.%d", g_tel_path, i);
        snprintf(new_path, sizeof(new_path), "%s.%d", g_tel_path, i + 1);
        if (access(old_path, F_OK) == 0 && rename(old_path, new_path) != 0) {
            LOG_WARN("Telemetry rotate: rename %s -> %s failed: %s",
                     old_path, new_path, strerror(errno));
        }
    }

    snprintf(new_path, sizeof(new_path), "%s.1", g_tel_path);
    if (rename(g_tel_path, new_path) != 0) {
        LOG_ERROR("Telemetry rotate: rename %s -> %s failed: %s",
                  g_tel_path, new_path, strerror(errno));
    }

    /* "ab"：二进制追加。文件里会有 0x00、0x0A 以及帧尾 55 55，
     * 任何按文本处理的假设都是错的。 */
    g_tel_fp = fopen(g_tel_path, "ab");
    if (!g_tel_fp) {
        LOG_ERROR("Telemetry rotate: reopen %s failed: %s", g_tel_path, strerror(errno));
    }
    g_tel_bytes = 0;
}

/* ==================== 写盘线程 ==================== */
static void* tel_writer_thread(void *arg)
{
    tel_slot_t slot;
    uint64_t reported_dropped = 0;

    (void)arg;
    LOG_INFO("Telemetry log writer thread started");

    while (true) {
        pthread_mutex_lock(&g_tel_queue_mutex);
        while (g_tel_count == 0 && !g_tel_stopping) {
            pthread_cond_wait(&g_tel_data_cond, &g_tel_queue_mutex);
        }
        /* 停止时先把队列里剩的帧写完再退出，避免丢尾部数据 */
        if (g_tel_count == 0 && g_tel_stopping) {
            pthread_mutex_unlock(&g_tel_queue_mutex);
            break;
        }

        memcpy(&slot, &g_tel_ring[g_tel_head], sizeof(slot));
        g_tel_head = (g_tel_head + 1) % TEL_QUEUE_SLOTS;
        g_tel_count--;
        uint64_t dropped = g_tel_dropped;
        pthread_mutex_unlock(&g_tel_queue_mutex);   /* 出队瞬间就放锁 */

        if (dropped != reported_dropped) {
            LOG_WARN("Telemetry log frames dropped (queue full): total=%llu",
                     (unsigned long long)dropped);
            reported_dropped = dropped;
        }

        /* 文件动作在文件锁里，队列锁已释放：快照复制再久也只是让队列积压，
         * 接收线程照样能把新帧塞进队列。这正是拆两把锁的目的。 */
        pthread_mutex_lock(&g_tel_file_mutex);
        if (g_tel_bytes + slot.len >= g_tel_max_size) {
            tel_rotate_locked();
        }
        if (g_tel_fp) {
            size_t n = fwrite(slot.data, 1, slot.len, g_tel_fp);
            if (n == slot.len) {
                g_tel_bytes += slot.len;
                fflush(g_tel_fp);   /* 3 帧/分钟，代价可忽略，保证掉电前已交给内核 */
            } else {
                /* 写失败（多为分区写满）。先强制轮转腾空间：unlink 最旧备份是真能
                 * 释放空间的（rename 与 fopen 本身不需要额外空间），再重写一次。
                 * 仍失败则【不推进 g_tel_bytes】—— 否则轮转阈值会永久错位，之后再也
                 * 不轮转，文件会一直长到把分区写满。 */
                LOG_ERROR("Telemetry log write failed: %s", strerror(errno));
                tel_rotate_locked();
                if (g_tel_fp) {
                    n = fwrite(slot.data, 1, slot.len, g_tel_fp);
                    if (n == slot.len) {
                        g_tel_bytes += slot.len;
                        fflush(g_tel_fp);
                    } else {
                        LOG_ERROR("Telemetry log write still failing after rotate: %s",
                                  strerror(errno));
                    }
                }
            }
        }
        pthread_mutex_unlock(&g_tel_file_mutex);
    }

    LOG_INFO("Telemetry log writer thread exited");
    return NULL;
}

/* ==================== 轮询线程 ==================== */
static void* tel_poll_thread(void *arg)
{
    static const uint8_t poll_ids[3] = {
        FPGA_MSG_TELEMETRY_POLL_V7A,
        FPGA_MSG_TELEMETRY_POLL_V7B1,
        FPGA_MSG_TELEMETRY_POLL_V7B2,
    };
    bool uart_down_reported = false;
    struct timespec deadline;

    (void)arg;
    LOG_INFO("Telemetry poll thread started (interval=%us)", g_tel_poll_sec);

    tel_now(&deadline);
    deadline.tv_sec += g_tel_poll_sec;

    while (true) {
        /* 用 cond_timedwait 而不是 sleep(interval)：sleep 期间收不到停止信号，
         * destroy() 就得等满一个轮询周期才能退出（现场表现为"发完 SIGTERM 进程卡住"，
         * 周期还是可配的，改大以后更糟；启动脚本若有超时会挨 SIGKILL 而丢数据）。 */
        pthread_mutex_lock(&g_tel_queue_mutex);
        while (!g_tel_stopping) {
            if (pthread_cond_timedwait(&g_tel_timer_cond, &g_tel_queue_mutex,
                                       &deadline) == ETIMEDOUT) {
                break;
            }
        }
        bool stopping = g_tel_stopping;
        pthread_mutex_unlock(&g_tel_queue_mutex);

        if (stopping) {
            break;
        }

        /* 推进绝对时刻而不是累加 sleep：每轮的零头长期累积会变成明显漂移 */
        deadline.tv_sec += g_tel_poll_sec;

        /* UART 未打开时不下发，也不重复报错。uart_client_send 失败时会自己打一条
         * ERROR，一分钟 3 条 = 4320 条/天，足以把只有 10MB 就轮转的文本日志刷满、
         * 把真正的故障记录挤出去。改为边沿触发：只在状态变化时各报一次。 */
        if (uart_client_get_state(&g_uart_client) != UART_STATE_OPEN) {
            if (!uart_down_reported) {
                LOG_WARN("Telemetry poll skipped: UART not open");
                uart_down_reported = true;
            }
            continue;
        }
        if (uart_down_reported) {
            LOG_INFO("Telemetry poll resumed: UART open");
            uart_down_reported = false;
        }

        /* 需求明确要求：轮询是本地的，与基带 TCP 链路状态无关，照常进行。
         * 不要"顺手"在这里加链路判断。 */
        for (int i = 0; i < 3; i++) {
            uint8_t payload = 0x00;
            uint8_t buf[16];
            fpga_message_t msg;
            int len;

            memset(&msg, 0, sizeof(msg));
            msg.msg_id = poll_ids[i];
            msg.payload = &payload;
            msg.payload_len = 1;

            len = fpga_encode_message(&msg, buf, sizeof(buf));
            if (len > 0) {
                /* 失败已在 uart_client_send 内部打过日志，这里不重复打 */
                uart_client_send(&g_uart_client, buf, (uint32_t)len);
                LOG_DEBUG("Sent telemetry poll 0x%02X", poll_ids[i]);
            } else {
                LOG_ERROR("Failed to encode telemetry poll 0x%02X", poll_ids[i]);
            }

            /* 三条命令之间留间隔：921600 下 7 字节只需约 76us，背靠背连发时 FPGA 侧
             * 若按"取完一帧才收下一帧"处理，可能在一次中断里连收三帧而丢帧。
             * 这里用 usleep（不可中断），最坏只多等两个间隔。 */
            if (i < 2 && g_tel_poll_gap_ms > 0) {
                usleep(g_tel_poll_gap_ms * 1000);
            }
        }
    }

    LOG_INFO("Telemetry poll thread exited");
    return NULL;
}

/* ==================== 对外接口 ==================== */

void telemetry_log_on_frame(const uint8_t *frame, uint32_t len)
{
    if (!frame || len == 0 || len > TEL_SLOT_MAX_FRAME) {
        return;
    }

    pthread_mutex_lock(&g_tel_queue_mutex);

    /* 销毁之后 UART 接收线程仍可能回调（main.c 先销毁本模块、后关 UART），
     * 此时必须静默返回，不能往已停用的队列里推数据。 */
    if (g_tel_stopping) {
        pthread_mutex_unlock(&g_tel_queue_mutex);
        return;
    }

    if (g_tel_count >= TEL_QUEUE_SLOTS) {
        /* 队列满：丢新帧而不是阻塞接收线程。接收线程的优先级高于日志完整性。 */
        g_tel_dropped++;
        pthread_mutex_unlock(&g_tel_queue_mutex);
        return;
    }

    memcpy(g_tel_ring[g_tel_tail].data, frame, len);
    g_tel_ring[g_tel_tail].len = (uint16_t)len;
    g_tel_tail = (g_tel_tail + 1) % TEL_QUEUE_SLOTS;
    g_tel_count++;

    pthread_cond_signal(&g_tel_data_cond);
    pthread_mutex_unlock(&g_tel_queue_mutex);
}

int telemetry_log_snapshot(const char *dst_path)
{
    FILE *src = NULL;
    FILE *dst = NULL;
    uint8_t buf[4096];
    int ret = SUCCESS;

    if (!dst_path) {
        return ERROR_INVALID_PARAM;
    }

    /* 持文件锁期间不会有新帧落盘，所以快照末尾必然停在某一帧的边界上——
     * 这正是相对 cp 的核心收益。 */
    pthread_mutex_lock(&g_tel_file_mutex);

    if (!g_tel_fp) {
        pthread_mutex_unlock(&g_tel_file_mutex);
        return ERROR_GENERAL;
    }

    /* 先把写缓冲落到文件，否则快照会缺最后几帧 */
    fflush(g_tel_fp);

    src = fopen(g_tel_path, "rb");
    if (!src) {
        ret = ERROR_GENERAL;
        goto out;
    }
    dst = fopen(dst_path, "wb");
    if (!dst) {
        ret = ERROR_GENERAL;
        goto out;
    }

    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), src);
        if (n > 0 && fwrite(buf, 1, n, dst) != n) {
            ret = ERROR_GENERAL;
            goto out;
        }
        if (n < sizeof(buf)) {
            if (ferror(src)) {
                ret = ERROR_GENERAL;
            }
            break;
        }
    }

    if (fflush(dst) != 0) {
        ret = ERROR_GENERAL;
    }

out:
    if (src) {
        fclose(src);
    }
    if (dst) {
        fclose(dst);
    }
    pthread_mutex_unlock(&g_tel_file_mutex);

    if (ret != SUCCESS) {
        LOG_WARN("Telemetry log snapshot to %s failed: %s", dst_path, strerror(errno));
    }
    return ret;
}

const char *telemetry_log_get_path(void)
{
    return g_tel_path;
}

/* 条件变量与线程创建失败时的公共收尾。调用者已确保没有线程在跑。 */
static void tel_init_fail_cleanup(void)
{
    if (g_tel_fp) {
        fclose(g_tel_fp);
        g_tel_fp = NULL;
    }
    if (g_tel_cond_valid) {
        pthread_cond_destroy(&g_tel_data_cond);
        pthread_cond_destroy(&g_tel_timer_cond);
        g_tel_cond_valid = false;
    }
}

int telemetry_log_init(void)
{
    pthread_condattr_t cattr;
    struct stat st;
    char *path_copy;
    const char *cfg_path;
    uint32_t cfg_max_size;
    uint32_t cfg_poll_sec;

    if (g_tel_inited) {
        return SUCCESS;
    }

    cfg_path = config_get_string("TELEMETRY_LOG_FILE", TELEMETRY_LOG_DEFAULT_PATH);
    if (cfg_path && cfg_path[0]) {
        strncpy(g_tel_path, cfg_path, sizeof(g_tel_path) - 1);
        g_tel_path[sizeof(g_tel_path) - 1] = '\0';
    }

    cfg_max_size = config_get_uint32("TELEMETRY_LOG_MAX_SIZE", 4 * 1024 * 1024);
    g_tel_max_size = (cfg_max_size > 0) ? (size_t)cfg_max_size : (size_t)(4 * 1024 * 1024);

    g_tel_max_backups = config_get_int("TELEMETRY_LOG_MAX_BACKUPS", 5);
    if (g_tel_max_backups < 0) {
        g_tel_max_backups = 0;
    }

    cfg_poll_sec = config_get_uint32("TELEMETRY_LOG_POLL_SEC", 60);
    /* 0 会让轮询线程死循环空转，退回默认值 */
    g_tel_poll_sec = (cfg_poll_sec > 0) ? cfg_poll_sec : 60;

    g_tel_poll_gap_ms = config_get_uint32("TELEMETRY_LOG_POLL_GAP_MS", 100);

    /* 建目录（照抄 logger_init 的做法：dirname 会改入参，必须先 strdup） */
    path_copy = strdup(g_tel_path);
    if (path_copy) {
        char *dir = dirname(path_copy);
        struct stat dir_st;
        if (stat(dir, &dir_st) == -1) {
            /* argv 数组传参，不经过 shell */
            char *mkdir_argv[] = { (char *)"mkdir", (char *)"-p", dir, NULL };
            char mkdir_out[256] = {0};
            if (shell_run(mkdir_argv, mkdir_out, sizeof(mkdir_out)) != 0) {
                LOG_ERROR("Failed to create telemetry log directory: %s", dir);
            }
        }
        free(path_copy);
    }

    /* 条件变量必须先设 CLOCK_MONOTONIC 再初始化：默认走 CLOCK_REALTIME，
     * 系统时间被回拨时"等 60 秒"会变成几十分钟。代价是不能用静态初始化器。 */
    if (pthread_condattr_init(&cattr) != 0) {
        LOG_ERROR("Failed to init telemetry cond attr");
        return ERROR_GENERAL;
    }
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    if (pthread_cond_init(&g_tel_data_cond, &cattr) != 0 ||
        pthread_cond_init(&g_tel_timer_cond, &cattr) != 0) {
        LOG_ERROR("Failed to init telemetry cond vars");
        pthread_condattr_destroy(&cattr);
        return ERROR_GENERAL;
    }
    pthread_condattr_destroy(&cattr);
    g_tel_cond_valid = true;

    g_tel_fp = fopen(g_tel_path, "ab");
    if (!g_tel_fp) {
        LOG_ERROR("Failed to open telemetry log %s: %s", g_tel_path, strerror(errno));
        tel_init_fail_cleanup();
        return ERROR_GENERAL;
    }

    /* 启动时就检查轮转：否则要等文件再长一倍才轮转，上限形同虚设 */
    g_tel_bytes = 0;
    if (stat(g_tel_path, &st) == 0 && st.st_size > 0) {
        g_tel_bytes = (size_t)st.st_size;
    }
    if (g_tel_bytes >= g_tel_max_size) {
        pthread_mutex_lock(&g_tel_file_mutex);
        tel_rotate_locked();
        pthread_mutex_unlock(&g_tel_file_mutex);
    }

    g_tel_head = 0;
    g_tel_tail = 0;
    g_tel_count = 0;
    g_tel_dropped = 0;
    g_tel_stopping = false;

    if (pthread_create(&g_tel_writer_tid, NULL, tel_writer_thread, NULL) != 0) {
        LOG_ERROR("Failed to create telemetry writer thread");
        tel_init_fail_cleanup();
        return ERROR_GENERAL;
    }
    g_tel_writer_valid = true;

    if (pthread_create(&g_tel_poll_tid, NULL, tel_poll_thread, NULL) != 0) {
        LOG_ERROR("Failed to create telemetry poll thread");
        pthread_mutex_lock(&g_tel_queue_mutex);
        g_tel_stopping = true;
        pthread_cond_signal(&g_tel_data_cond);
        pthread_mutex_unlock(&g_tel_queue_mutex);
        pthread_join(g_tel_writer_tid, NULL);
        g_tel_writer_valid = false;
        tel_init_fail_cleanup();
        return ERROR_GENERAL;
    }
    g_tel_poll_valid = true;

    g_tel_inited = true;
    LOG_INFO("Telemetry log initialized (path=%s, poll=%us, gap=%ums, max=%zu B, backups=%d)",
             g_tel_path, g_tel_poll_sec, g_tel_poll_gap_ms, g_tel_max_size, g_tel_max_backups);
    return SUCCESS;
}

void telemetry_log_destroy(void)
{
    if (!g_tel_inited) {
        return;
    }

    /* 先置停止位并唤醒两个线程。回调（UART 接收线程）也在这把队列锁里检查
     * g_tel_stopping，所以置位之后即使接收线程还在跑，也只会静默返回。 */
    pthread_mutex_lock(&g_tel_queue_mutex);
    g_tel_stopping = true;
    pthread_cond_signal(&g_tel_data_cond);
    pthread_cond_signal(&g_tel_timer_cond);
    pthread_mutex_unlock(&g_tel_queue_mutex);

    if (g_tel_poll_valid) {
        pthread_join(g_tel_poll_tid, NULL);
        g_tel_poll_valid = false;
    }
    if (g_tel_writer_valid) {
        pthread_join(g_tel_writer_tid, NULL);   /* 内部写完残余帧后才退出 */
        g_tel_writer_valid = false;
    }

    pthread_mutex_lock(&g_tel_file_mutex);
    if (g_tel_fp) {
        fflush(g_tel_fp);
        fclose(g_tel_fp);
        g_tel_fp = NULL;
    }
    pthread_mutex_unlock(&g_tel_file_mutex);

    /* 互斥量与条件变量都不销毁：销毁之后 UART 接收线程若仍回调
     * telemetry_log_on_frame（main.c 先销毁本模块、后关 UART），会锁到已销毁的
     * 互斥量（UB）。留给进程退出由内核回收，比留一个 use-after-destroy 的窗口划算。 */
    g_tel_inited = false;

    LOG_INFO("Telemetry log destroyed");
}
