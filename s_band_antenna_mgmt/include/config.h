#ifndef CONFIG_H
#define CONFIG_H

#include "common.h"

/* 配置文件加载 */
int config_load(const char *config_file);

/* 获取配置项 */
const char* config_get_string(const char *key, const char *default_value);
int config_get_int(const char *key, int default_value);
uint32_t config_get_uint32(const char *key, uint32_t default_value);

#endif /* CONFIG_H */
