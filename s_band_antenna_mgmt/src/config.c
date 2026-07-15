#include "config.h"
#include "logger.h"

#define MAX_CONFIG_ITEMS 64
#define MAX_LINE_LEN 256

typedef struct {
    char key[64];
    char value[192];
} config_item_t;

static config_item_t g_config_items[MAX_CONFIG_ITEMS];
static int g_config_count = 0;

/* 全局配置实例 */
global_config_t g_config = {
    .bbu_ip = "10.10.10.6",
    .bbu_port = 30000,
    .paau_ip = "10.10.10.8",
    .reconnect_interval_ms = 3000,
    .heartbeat_interval_ms = 3000,
    .log_level = LOG_LEVEL_INFO
};

static void trim_whitespace(char *str)
{
    char *start = str;
    char *end;

    while (*start == ' ' || *start == '\t') start++;

    if (*start == 0) {
        *str = 0;
        return;
    }

    end = start + strlen(start) - 1;
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        end--;
    }
    *(end + 1) = 0;

    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
}

int config_load(const char *config_file)
{
    FILE *fp = fopen(config_file, "r");
    if (!fp) {
        fprintf(stderr, "Failed to open config file: %s\n", config_file);
        return ERROR_GENERAL;
    }

    char line[MAX_LINE_LEN];
    g_config_count = 0;

    while (fgets(line, sizeof(line), fp) && g_config_count < MAX_CONFIG_ITEMS) {
        trim_whitespace(line);

        /* 跳过注释和空行 */
        if (line[0] == '#' || line[0] == 0) {
            continue;
        }

        /* 解析键值对 */
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }

        *eq = 0;
        char *key = line;
        char *value = eq + 1;

        trim_whitespace(key);
        trim_whitespace(value);

        strncpy(g_config_items[g_config_count].key, key, sizeof(g_config_items[0].key) - 1);
        strncpy(g_config_items[g_config_count].value, value, sizeof(g_config_items[0].value) - 1);
        g_config_count++;
    }

    fclose(fp);

    /* 应用配置到全局配置结构 */
    strncpy(g_config.bbu_ip, config_get_string("BBU_IP", "10.10.10.6"), sizeof(g_config.bbu_ip) - 1);
    g_config.bbu_port = config_get_int("BBU_PORT", 30000);
    strncpy(g_config.paau_ip, config_get_string("PAAU_IP", "10.10.10.8"), sizeof(g_config.paau_ip) - 1);
    g_config.reconnect_interval_ms = config_get_int("RECONNECT_INTERVAL_MS", 3000);
    g_config.heartbeat_interval_ms = config_get_int("HEARTBEAT_INTERVAL_MS", 3000);

    const char *log_level_str = config_get_string("LOG_LEVEL", "INFO");
    if (strcmp(log_level_str, "DEBUG") == 0) {
        g_config.log_level = LOG_LEVEL_DEBUG;
    } else if (strcmp(log_level_str, "INFO") == 0) {
        g_config.log_level = LOG_LEVEL_INFO;
    } else if (strcmp(log_level_str, "WARN") == 0) {
        g_config.log_level = LOG_LEVEL_WARN;
    } else if (strcmp(log_level_str, "ERROR") == 0) {
        g_config.log_level = LOG_LEVEL_ERROR;
    }

    /* 加载日志轮转配置 */
    g_config.log_max_size = (size_t)config_get_int("LOG_MAX_SIZE", 10485760);  // 默认10MB
    g_config.log_max_backups = config_get_int("LOG_MAX_BACKUPS", 5);
    g_config.log_rotate_on_startup = config_get_int("LOG_ROTATE_ON_STARTUP", 1) != 0;

    printf("Config loaded: %d items\n", g_config_count);
    return SUCCESS;
}

const char* config_get_string(const char *key, const char *default_value)
{
    for (int i = 0; i < g_config_count; i++) {
        if (strcmp(g_config_items[i].key, key) == 0) {
            return g_config_items[i].value;
        }
    }
    return default_value;
}

int config_get_int(const char *key, int default_value)
{
    const char *value = config_get_string(key, NULL);
    if (value) {
        return atoi(value);
    }
    return default_value;
}

uint32_t config_get_uint32(const char *key, uint32_t default_value)
{
    const char *value = config_get_string(key, NULL);
    if (value) {
        /* 支持十六进制格式 (0x开头) */
        if (strncmp(value, "0x", 2) == 0 || strncmp(value, "0X", 2) == 0) {
            return (uint32_t)strtoul(value, NULL, 16);
        }
        return (uint32_t)strtoul(value, NULL, 10);
    }
    return default_value;
}
