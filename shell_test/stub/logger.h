/* 测试用的 logger.h 桩：只提供 shell_util.c 需要的日志宏，
 * 避免把整个项目的依赖（config/main 全局）拖进单元测试。 */
#ifndef LOGGER_H_STUB
#define LOGGER_H_STUB

#include <stdio.h>

#define LOG_ERROR(fmt, ...) fprintf(stderr, "[E] " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[W] " fmt "\n", ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  fprintf(stderr, "[I] " fmt "\n", ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) do { } while (0)

#endif /* LOGGER_H_STUB */
