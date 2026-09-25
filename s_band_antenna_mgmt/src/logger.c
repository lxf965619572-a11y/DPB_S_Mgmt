#include <stdarg.h>
#include <sys/stat.h>
#include <libgen.h>
#include <string.h>
#include "logger.h"
#include "shell_util.h"

static FILE *g_log_file = NULL;
static log_level_t g_log_level = LOG_LEVEL_INFO;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 保存日志文件路径 */
static char g_log_file_path[256] = {0};

/* 写入计数器（用于运行时定期检查） */
static size_t g_log_write_count = 0;

static const char* log_level_str[] = {
    "DEBUG",
    "INFO",
    "WARN",
    "ERROR"
};

/**
 * 执行日志轮转
 * 注意：调用前必须持有 g_log_mutex
 * 返回：SUCCESS 或 ERROR_GENERAL
 */
static int logger_rotate(void)
{
    char old_path[512];
    char new_path[512];
    int i;
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_buf[64];

    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);

    /* 记录轮转开始 */
    if (g_log_file) {
        fprintf(g_log_file, "[%s] [INFO] Starting log rotation...\n", time_buf);
        fflush(g_log_file);
    }

    /* 1. 关闭当前日志文件 */
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }

    /* 2. 删除最旧的备份文件（如果存在） */
    if (g_config.log_max_backups > 0) {
        snprintf(old_path, sizeof(old_path), "%s.%d",
                 g_log_file_path, g_config.log_max_backups);
        if (access(old_path, F_OK) == 0) {
            if (unlink(old_path) != 0) {
                fprintf(stderr, "[%s] Warning: Failed to delete old backup: %s (errno=%d)\n",
                        time_buf, old_path, errno);
            }
        }
    }

    /* 3. 重命名现有备份文件：log.N-1 → log.N, ..., log.1 → log.2 */
    for (i = g_config.log_max_backups - 1; i >= 1; i--) {
        snprintf(old_path, sizeof(old_path), "%s.%d", g_log_file_path, i);
        snprintf(new_path, sizeof(new_path), "%s.%d", g_log_file_path, i + 1);

        if (access(old_path, F_OK) == 0) {
            if (rename(old_path, new_path) != 0) {
                fprintf(stderr, "[%s] Warning: Failed to rename %s to %s (errno=%d)\n",
                        time_buf, old_path, new_path, errno);
            }
        }
    }

    /* 4. 重命名当前日志文件：log → log.1 */
    snprintf(new_path, sizeof(new_path), "%s.1", g_log_file_path);
    if (rename(g_log_file_path, new_path) != 0) {
        fprintf(stderr, "[%s] Error: Failed to rename current log file: %s (errno=%d)\n",
                time_buf, g_log_file_path, errno);
        /* 尝试重新打开原文件 */
        g_log_file = fopen(g_log_file_path, "a");
        return ERROR_GENERAL;
    }

    /* 5. 创建新的日志文件 */
    g_log_file = fopen(g_log_file_path, "a");
    if (!g_log_file) {
        fprintf(stderr, "[%s] Error: Failed to create new log file: %s (errno=%d)\n",
                time_buf, g_log_file_path, errno);
        return ERROR_GENERAL;
    }

    /* 6. 记录轮转成功 */
    fprintf(g_log_file, "[%s] [INFO] Log rotation completed, old log saved as %s\n",
            time_buf, new_path);
    fflush(g_log_file);

    return SUCCESS;
}

/**
 * 检查日志文件大小并在必要时轮转
 * 注意：调用前必须持有 g_log_mutex
 * 返回：SUCCESS 或 ERROR_GENERAL
 */
static int logger_check_and_rotate(void)
{
    struct stat st;

    /* 检查文件是否存在 */
    if (stat(g_log_file_path, &st) != 0) {
        return SUCCESS;  // 文件不存在，无需轮转
    }

    /* 检查文件大小 */
    if ((size_t)st.st_size < g_config.log_max_size) {
        return SUCCESS;  // 未超过阈值，无需轮转
    }

    /* 执行轮转 */
    return logger_rotate();
}

int logger_init(const char *log_file, log_level_t level)
{
    g_log_level = level;

    if (log_file) {
        /* 保存日志文件路径 */
        strncpy(g_log_file_path, log_file, sizeof(g_log_file_path) - 1);

        /* 创建日志目录（如果不存在） */
        char *log_file_copy = strdup(log_file);
        if (log_file_copy) {
            char *dir = dirname(log_file_copy);
            struct stat st = {0};

            if (stat(dir, &st) == -1) {
                /* 目录不存在，创建它 */
                /* argv 数组传参，不经过 shell */
                char *mkdir_argv[] = { (char *)"mkdir", (char *)"-p", dir, NULL };
                char mkdir_out[256] = {0};
                if (shell_run(mkdir_argv, mkdir_out, sizeof(mkdir_out)) != 0) {
                    fprintf(stderr, "Failed to create log directory: %s\n", dir);
                    free(log_file_copy);
                    return ERROR_GENERAL;
                }
            }
            free(log_file_copy);
        }

        /* 启动时检查并轮转（如果启用） */
        if (g_config.log_rotate_on_startup) {
            struct stat st;
            if (stat(g_log_file_path, &st) == 0) {
                if ((size_t)st.st_size >= g_config.log_max_size) {
                    fprintf(stdout, "Log file size (%.2f MB) exceeds limit (%.2f MB), rotating...\n",
                            st.st_size / (1024.0 * 1024.0),
                            g_config.log_max_size / (1024.0 * 1024.0));

                    /* 临时打开文件以执行轮转 */
                    pthread_mutex_lock(&g_log_mutex);
                    g_log_file = fopen(log_file, "a");
                    logger_rotate();
                    pthread_mutex_unlock(&g_log_mutex);
                }
            }
        }

        g_log_file = fopen(log_file, "a");
        if (!g_log_file) {
            fprintf(stderr, "Failed to open log file: %s\n", log_file);
            return ERROR_GENERAL;
        }
    }

    return SUCCESS;
}

void logger_log(log_level_t level, const char *file, int line, const char *fmt, ...)
{
    if (level < g_log_level) {
        return;
    }

    time_t now;
    struct tm *tm_info;
    char time_buf[64];
    char log_buf[1024];

    time(&now);
    tm_info = localtime(&now);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);

    va_list args;
    va_start(args, fmt);
    vsnprintf(log_buf, sizeof(log_buf), fmt, args);
    va_end(args);

    pthread_mutex_lock(&g_log_mutex);

    /* 定期检查文件大小（每1000次写入检查一次，避免频繁stat） */
    g_log_write_count++;
    if (g_log_write_count >= 1000) {
        g_log_write_count = 0;
        logger_check_and_rotate();
    }

    /* 输出到控制台 */
    fprintf(stdout, "[%s] [%s] %s\n", time_buf, log_level_str[level], log_buf);
    fflush(stdout);

    /* 输出到文件 */
    if (g_log_file) {
        fprintf(g_log_file, "[%s] [%s] [%s:%d] %s\n",
                time_buf, log_level_str[level], file, line, log_buf);
        fflush(g_log_file);
    }

    pthread_mutex_unlock(&g_log_mutex);
}

void logger_close(void)
{
    /* 必须与 logger_log 共用同一把锁，否则存在如下竞态：
     * logger_log 在 :217 判断 g_log_file 非空后、尚未 fprintf 时，
     * 本函数把文件 fclose 并置 NULL → 对已关闭的 FILE* 写入（use-after-free）。 */
    pthread_mutex_lock(&g_log_mutex);
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
    pthread_mutex_unlock(&g_log_mutex);
}
