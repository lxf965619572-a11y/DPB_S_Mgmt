#ifndef LOGGER_H
#define LOGGER_H

#include "common.h"

/* 日志初始化 */
int logger_init(const char *log_file, log_level_t level);

/* 日志输出 */
void logger_log(log_level_t level, const char *file, int line, const char *fmt, ...);

/* 刷新日志到磁盘 */
void logger_flush(void);

/* 日志关闭 */
void logger_close(void);

/* 日志宏定义 */
#define LOG_DEBUG(fmt, ...) logger_log(LOG_LEVEL_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  logger_log(LOG_LEVEL_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  logger_log(LOG_LEVEL_WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) logger_log(LOG_LEVEL_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#endif /* LOGGER_H */
