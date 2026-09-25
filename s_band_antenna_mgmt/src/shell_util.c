#include "shell_util.h"
#include "logger.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

int shell_run(char *const argv[], char *out, size_t out_size)
{
    if (!argv || !argv[0] || !out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        LOG_ERROR("shell_run: pipe failed: %s", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("shell_run: fork failed: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* 子进程：stdout 与 stderr 都接到管道，然后直接 execvp，不经过 shell。
         * 用 _exit 而非 exit，避免在子进程里跑父进程注册的 atexit/stdio 清理。 */
        if (dup2(pipefd[1], STDOUT_FILENO) < 0 ||
            dup2(pipefd[1], STDERR_FILENO) < 0) {
            _exit(127);
        }
        close(pipefd[0]);
        close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(127);     /* exec 失败（如命令不存在） */
    }

    /* 父进程：只读，写端必须关掉，否则读到子进程退出也等不到 EOF */
    close(pipefd[1]);

    size_t used = 0;
    char scratch[256];
    for (;;) {
        char *dst;
        size_t cap;
        if (used < out_size - 1) {
            dst = out + used;
            cap = out_size - 1 - used;
        } else {
            /* 缓冲已满仍要继续读：否则子进程写满管道后会永久阻塞，
             * 父进程 waitpid 也就永远不返回。 */
            dst = scratch;
            cap = sizeof(scratch);
        }

        ssize_t n = read(pipefd[0], dst, cap);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;      /* EOF：子进程已关闭写端 */
        }
        if (used < out_size - 1) {
            used += (size_t)n;
        }
    }
    out[used] = '\0';
    close(pipefd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        /* 被信号打断则重试 */
    }
    return status;
}

bool shell_safe_name(const char *name)
{
    if (!name || name[0] == '\0') {
        return false;
    }

    size_t len = strlen(name);
    if (len > 255) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        bool ok = (c >= 'A' && c <= 'Z') ||
                  (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-';
        if (!ok) {
            return false;   /* 含 '/'、'\\'、'..' 中的点虽允许但单点无妨，分隔符一律拒绝 */
        }
    }

    /* 单独校验：不允许整体为 "." 或 ".."（点号在前一步是允许的） */
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return false;
    }

    return true;
}

bool shell_safe_relpath(const char *path)
{
    if (!path || path[0] == '\0') {
        return false;
    }

    size_t len = strlen(path);
    if (len > 255) {
        return false;
    }

    /* 逐段检查：任何一段为 ".." 即拒绝（阻断 ../../ 穿越） */
    const char *p = path;
    while (*p != '\0') {
        const char *seg_end = strchr(p, '/');
        size_t seg_len = seg_end ? (size_t)(seg_end - p) : strlen(p);

        if (seg_len == 2 && p[0] == '.' && p[1] == '.') {
            return false;
        }

        if (!seg_end) {
            break;
        }
        p = seg_end + 1;
    }

    return true;
}
