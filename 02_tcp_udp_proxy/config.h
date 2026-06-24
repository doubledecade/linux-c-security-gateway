//
// Created by iFOURYTWOLF on 26-6-7.
//

#ifndef LINUX_C_SECURITY_GATEWAY_CONFIG_H
#define LINUX_C_SECURITY_GATEWAY_CONFIG_H
#include "log.h"

#define MAX_LISTENERS 16
#define LISTEN_TYPE_NAME_MAX 32

typedef enum listen_proto {
    LISTEN_PROTO_TCP = 0,
    LISTEN_PROTO_UDP
} listen_proto_t;

typedef enum service_type {
    SERVICE_TYPE_ECHO = 0,
    SERVICE_TYPE_PROXY
} service_type_t;

typedef struct listener_config {
    listen_proto_t proto;
    service_type_t service_type;
    char listen_type[LISTEN_TYPE_NAME_MAX];
    char listen_ip[64];
    int listen_port;
    char type[LISTEN_TYPE_NAME_MAX];
    char target_ip[64];
    int target_port;
} listener_config_t;

typedef struct server_config {
    listener_config_t listeners[MAX_LISTENERS];
    int listener_count;
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
