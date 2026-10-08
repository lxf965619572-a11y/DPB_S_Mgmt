#ifndef TELEMETRY_LOG_H
#define TELEMETRY_LOG_H

#include "common.h"

/* 遥测日志默认路径。log_upload 打包时读的是同一个配置项，默认值必须与此处一致，
 * 故集中定义，避免两处各自硬编码的字符串日后漂移。 */
#define TELEMETRY_LOG_DEFAULT_PATH  "logs/telemetry.bin"

/* 初始化：读配置、建目录、打开文件、启动轮询线程与写盘线程。
 *
 * 必须在 uart_client_open() 之后调用（轮询线程要经 UART 下发命令）。
 * 失败不致命：遥测日志是旁路功能，调用方打一条 WARN 后继续运行即可。
 * 重复调用幂等。 */
int telemetry_log_init(void);

/* UART 接收线程的回调，装配到 uart_client_t::on_telemetry_log_frame。
 *
 * 【同步语义】frame 指向接收线程内的 static frame_buffer，本函数返回后循环末尾的
 * memmove 会立刻覆盖它，所以必须在返回前把整帧拷进队列，不能只保存指针。
 * 内部不做任何磁盘 I/O、不取日志锁，只做一次带锁 memcpy —— 接收线程同时还承载
 * 0xEB90 状态帧到告警判据的路径，绝不能被写盘阻塞。 */
void telemetry_log_on_frame(const uint8_t *frame, uint32_t len);

/* 把当前遥测日志整份复制到 dst_path，供日志上传打包使用。
 *
 * 持文件锁复制，与轮转互斥，因此快照必然落在整帧边界上 —— 直接 cp 做不到这一点：
 * cp 读的是字节流，可能把最后一帧抄一半，得到一个尾部截断的二进制记录。
 *
 * 返回 ERROR_GENERAL 表示未初始化或复制失败，调用方应降级为只传文本日志。 */
int telemetry_log_snapshot(const char *dst_path);

/* 遥测日志文件路径。未初始化时返回默认路径，不会返回 NULL。 */
const char *telemetry_log_get_path(void);

/* 停止两个线程并 join、flush 并关闭文件。可重复调用；未初始化时安全。 */
void telemetry_log_destroy(void);

#endif /* TELEMETRY_LOG_H */
