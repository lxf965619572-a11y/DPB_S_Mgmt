#ifndef SHELL_UTIL_H
#define SHELL_UTIL_H

#include "common.h"
#include <stddef.h>

/*
 * 以 argv 数组方式执行外部命令，并捕获其 stdout+stderr。
 *
 * 与 popen()/system() 的关键区别：**不经过 shell 解析**。参数按数组原样传给
 * execvp，因此报文里的字段（文件名 file_name、版本号 file_ver、路径 store_path 等）
 * 无法注入 `"` `;` `` ` `` `$()` 等元字符。这是替代
 *     popen("tar -xzf \"%s\" -C \"%s\"")   /   system("rm -rf \"%s\"")
 * 这类写法的标准做法（原写法只用双引号包裹，挡不住 `$()` 与反引号，引号本身也可闭合逃逸）。
 *
 * argv      : 以 NULL 结尾的参数数组；argv[0] 为可执行文件名，经 PATH 查找（execvp）
 * out       : 输出缓冲，写入子进程合并后的 stdout+stderr，始终以 NUL 结尾
 * out_size  : out 的容量；子进程输出超出部分会被读取丢弃，不会阻塞子进程
 *
 * 返回 waitpid 的原始状态（与 system()/pclose() 同约定）：0 表示命令成功；
 * 返回 -1 表示 pipe/fork 失败（此时 out 被置为空串）。
 */
int shell_run(char *const argv[], char *out, size_t out_size);

/*
 * 校验来自北向报文的“名称”字段（文件名、版本号等）。
 * 只接受 [A-Za-z0-9._-]，长度 1..255，且不允许出现路径分隔符，
 * 因此 ".."、"../../etc"、"a/b" 这类写法一律被拒绝。
 */
bool shell_safe_name(const char *name);

/*
 * 校验来自北向报文的“路径”字段：允许 '/'，但长度上限 255，
 * 且任何一段都不允许是 ".."，用于阻断路径穿越。
 * 另拒绝控制字符（含换行），该值会被拼进 FTP URL 并直接打进日志。
 */
bool shell_safe_relpath(const char *path);

/*
 * 校验压缩包(tar)成员名，用于在解压前拦截路径穿越。
 *
 * 成员名是解压前唯一能判定的穿越特征：压缩包内容本身在解压前没有可信参照。
 * GNU tar 会剥掉成员名开头的 '/' 与 '../'，并对含 '..' 的成员直接报错拒绝
 * （实测 1.35 行为），所以本函数在 GNU tar 上属冗余兜底；换成 busybox/BSD tar
 * 或旧版本时才是真正的拦截层。
 * 按成员名逐字判断，比照 shell_safe_relpath 但更严：
 *   - 拒绝绝对路径（开头的 '/'）；
 *   - 拒绝任何一段为 ".."；
 *   - 拒绝控制字符与空串；
 *   - 允许结尾的 '/'（目录成员），允许中间的单点段（"./a"）。
 * 注意：只查名字，不查成员类型（符号链接/硬链接成员不在本函数职责内）。
 */
bool shell_safe_tar_member(const char *member);

#endif /* SHELL_UTIL_H */
