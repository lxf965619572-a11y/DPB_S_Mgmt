/*
 * shell_util 单元测试：验证命令注入已被消除，且旧写法确实可被注入（对照组）。
 * 编译：gcc -I stub -I ../s_band_antenna_mgmt/include -o t_shell t_shell.c ../s_band_antenna_mgmt/src/shell_util.c
 */
#include "shell_util.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, msg) do {                                \
    if (cond) { printf("    PASS  %s\n", (msg)); }           \
    else      { printf("    FAIL  %s\n", (msg)); failures++; } \
} while (0)

static bool file_exists(const char *p)
{
    struct stat sb;
    return stat(p, &sb) == 0;
}

int main(void)
{
    /* 恶意"文件名"：闭合双引号后用 ; 分隔，追加一条命令。
     * 这正是原代码 popen("tar ... \"%s\"") 会中招的构造。 */
    const char *evil = "aaa\"; touch /tmp/PWNED_OLD; echo \"bbb";

    printf("=== 1. 对照组：旧写法（popen + 双引号包裹）会被注入 ===\n");
    {
        unlink("/tmp/PWNED_OLD");
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "echo \"%s\"", evil);   /* 旧代码的构造方式 */
        FILE *fp = popen(cmd, "r");
        if (fp) {
            char b[256];
            size_t n = fread(b, 1, sizeof(b) - 1, fp);
            b[n] = '\0';
            pclose(fp);
        }
        CHECK(file_exists("/tmp/PWNED_OLD"),
              "旧写法确实执行了注入的命令（说明该构造是真实可利用的）");
    }

    printf("=== 2. 新写法 shell_run：同样字符串只作普通参数 ===\n");
    {
        unlink("/tmp/PWNED_NEW");
        /* 模拟把北向字段当参数传给 tar：tar 不存在也无妨，重点是注入不成立 */
        char evil2[] = "aaa\"; touch /tmp/PWNED_NEW; echo \"bbb";
        char *argv[] = { (char *)"echo", evil2, NULL };
        char out[512] = {0};
        int st = shell_run(argv, out, sizeof(out));

        CHECK(st == 0, "echo 正常返回 0");
        CHECK(strstr(out, "touch /tmp/PWNED_NEW") != NULL,
              "元字符原样出现在输出中（被当作普通参数，未解析）");
        CHECK(!file_exists("/tmp/PWNED_NEW"), "注入的命令未被执行");
    }

    printf("=== 3. shell_run 的其他行为 ===\n");
    {
        char out[256] = {0};
        char *ok[] = { (char *)"echo", (char *)"hello", NULL };
        CHECK(shell_run(ok, out, sizeof(out)) == 0, "成功命令返回 0");
        CHECK(strcmp(out, "hello\n") == 0, "stdout 被正确捕获");

        char out2[256] = {0};
        char *bad[] = { (char *)"false", NULL };
        CHECK(shell_run(bad, out2, sizeof(out2)) != 0, "失败命令返回非 0");

        char out3[256] = {0};
        char *missing[] = { (char *)"this_command_does_not_exist_xyz", NULL };
        int st3 = shell_run(missing, out3, sizeof(out3));
        CHECK(st3 != 0, "不存在的命令返回非 0（不崩溃）");

        char out4[256] = {0};
        char *err[] = { (char *)"sh", (char *)"-c", (char *)"echo to-stderr 1>&2", NULL };
        shell_run(err, out4, sizeof(out4));
        CHECK(strstr(out4, "to-stderr") != NULL, "stderr 也被捕获（等价于旧的 2>&1）");

        /* 输出远超缓冲区时不能死锁 */
        char small[16] = {0};
        char *big[] = { (char *)"sh", (char *)"-c", (char *)"seq 1 20000", NULL };
        CHECK(shell_run(big, small, sizeof(small)) == 0,
              "输出远超缓冲区时不死锁（子进程被持续排空）");
    }

    printf("=== 4. shell_safe_name ===\n");
    {
        CHECK(shell_safe_name("v1.0.0"), "拒绝无关：'v1.0.0' 通过");
        CHECK(shell_safe_name("FPGA_v2.1-rc1"), "'FPGA_v2.1-rc1' 通过");
        CHECK(!shell_safe_name(""), "空串被拒（否则 version_dir 会退化成版本根目录）");
        CHECK(!shell_safe_name(".."), "'..' 被拒");
        CHECK(!shell_safe_name("../.."), "'../..' 被拒");
        CHECK(!shell_safe_name("../../etc/passwd"), "路径穿越被拒");
        CHECK(!shell_safe_name("a/b"), "含 '/' 被拒");
        CHECK(!shell_safe_name("v1; rm -rf /"), "含 ';' 与空格被拒");
        CHECK(!shell_safe_name("v1$(id)"), "含 '$(' 被拒");
        CHECK(!shell_safe_name("v1`id`"), "含反引号被拒");
    }

    printf("=== 5. shell_safe_relpath ===\n");
    {
        CHECK(shell_safe_relpath("/logs/2026-09"), "正常路径通过");
        CHECK(shell_safe_relpath("logs/antenna"), "相对路径通过");
        CHECK(!shell_safe_relpath("../../etc"), "路径穿越被拒");
        CHECK(!shell_safe_relpath("logs/../../etc"), "中间段 '..' 被拒");
        CHECK(!shell_safe_relpath(""), "空串被拒");
    }

    printf("\n==== %s（失败 %d 项）====\n", failures == 0 ? "全部通过" : "存在失败", failures);
    return failures == 0 ? 0 : 1;
}
