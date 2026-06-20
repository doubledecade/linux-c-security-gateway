//
// Created by iFOURYTWOLF on 26-6-7.
//

#ifndef LINUX_C_SECURITY_GATEWAY_CONFIG_H
#define LINUX_C_SECURITY_GATEWAY_CONFIG_H
#include "log.h"

typedef struct server_config {
    char listen_ip[64];
    int listen_port;
    int worker_num;
    app_log_level_t log_level;
    char log_file[256];
} server_config_t;

int load_config(const char *path);
int reload_config(const char *path);
void free_config(void);
/* 新增：获取当前配置快照 */
int get_config_snapshot(server_config_t *out);
#endif //LINUX_C_SECURITY_GATEWAY_CONFIG_H
