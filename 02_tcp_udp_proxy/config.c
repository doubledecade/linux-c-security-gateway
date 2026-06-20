//
// Created by iFOURYTWOLF on 26-6-7.
//

#include "config.h"
#include <stdio.h>
#include <ctype.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
static server_config_t *g_config;

static pthread_rwlock_t g_config_lock = PTHREAD_RWLOCK_INITIALIZER;
static char *trim(char *s)
{
    char *end;

    while (*s && isspace((unsigned char)*s)) {
        s++;
    }

    if (*s == '\0') {
        return s;
    }

    end = s + strlen(s) - 1;

    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }

    return s;
}

static int parse_log_level(const char *s, app_log_level_t *level)
{
    if (strcasecmp(s, "debug") == 0) {
        *level = APP_LOG_DEBUG;
        return 0;
    }

    if (strcasecmp(s, "info") == 0) {
        *level = APP_LOG_INFO;
        return 0;
    }

    if (strcasecmp(s, "warn") == 0 || strcasecmp(s, "warning") == 0) {
        *level = APP_LOG_WARN;
        return 0;
    }

    if (strcasecmp(s, "error") == 0) {
        *level = APP_LOG_ERROR;
        return 0;
    }

    return -1;
}

static void config_set_default(server_config_t *cfg)
{
    snprintf(cfg->listen_ip, sizeof(cfg->listen_ip), "0.0.0.0");
    cfg->listen_port = 8080;
    cfg->worker_num = 4;
    cfg->log_level = APP_LOG_INFO;
    snprintf(cfg->log_file, sizeof(cfg->log_file), "/tmp/myserver.log");
}

static int parse_config_file(const char *path, server_config_t *cfg)
{

    config_set_default(cfg);
    FILE *fp;
    char line[512];
    int line_no = 0;

    fp = fopen(path, "r");
    if (fp == NULL) {
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p;
        char *key;
        char *value;
        char *eq;

        line_no++;

        p = trim(line);

        /*
         * 跳过空行和注释行
         */
        if (*p == '\0' || *p == '#') {
            continue;
        }

        eq = strchr(p, '=');
        if (eq == NULL) {
            fprintf(stderr, "config error: line %d missing '='\n", line_no);
            fclose(fp);
            return -1;
        }

        *eq = '\0';

        key = trim(p);
        value = trim(eq + 1);

        if (*key == '\0' || *value == '\0') {
            fprintf(stderr, "config error: line %d empty key or value\n", line_no);
            fclose(fp);
            return -1;
        }

        if (strcmp(key, "listen_ip") == 0) {
            snprintf(cfg->listen_ip, sizeof(cfg->listen_ip), "%s", value);
        } else if (strcmp(key, "listen_port") == 0) {
            cfg->listen_port = atoi(value);
            if (cfg->listen_port <= 0 || cfg->listen_port > 65535) {
                fprintf(stderr, "config error: invalid listen_port: %s\n", value);
                fclose(fp);
                return -1;
            }
        } else if (strcmp(key, "worker_num") == 0) {
            cfg->worker_num = atoi(value);
            if (cfg->worker_num <= 0 || cfg->worker_num > 128) {
                fprintf(stderr, "config error: invalid worker_num: %s\n", value);
                fclose(fp);
                return -1;
            }
        } else if (strcmp(key, "log_level") == 0) {
            if (parse_log_level(value, &cfg->log_level) == -1) {
                fprintf(stderr, "config error: invalid log_level: %s\n", value);
                fclose(fp);
                return -1;
            }
        } else if (strcmp(key, "log_file") == 0) {
            snprintf(cfg->log_file, sizeof(cfg->log_file), "%s", value);
        } else {
            fprintf(stderr, "config warning: unknown key '%s' at line %d\n",
                    key, line_no);
        }
    }

    fclose(fp);
    return 0;
}

int reload_config(const char *path)
{
    server_config_t *old_config;
    server_config_t *new_config = (server_config_t *)malloc(sizeof(server_config_t));
    if (new_config == NULL) {
        fprintf(stderr, "failed to allocate memory for new config\n");
        return -1;
    }

    if (parse_config_file(path, new_config) == -1) {
        free(new_config);
        return -1;
    }
    pthread_rwlock_wrlock(&g_config_lock);
    old_config = g_config;
    g_config = new_config;
    pthread_rwlock_unlock(&g_config_lock);
    free(old_config);

    return 0;
}

int load_config(const char *path)
{
    server_config_t *cfg;
    if (path == NULL || path[0] == '\0') {
        fprintf(stderr, "invalid config path\n");
        return -1;
    }

    cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) {
        fprintf(stderr, "failed to allocate memory for config\n");
        return -1;
    }

    if (parse_config_file(path, cfg) == -1) {
        free(cfg);
        return -1;
    }
    pthread_rwlock_wrlock(&g_config_lock);

    if (g_config != NULL) {
        pthread_rwlock_unlock(&g_config_lock);
        fprintf(stderr, "config already loaded\n");
        free(cfg);
        return -1;
    }
    g_config = cfg;
    pthread_rwlock_unlock(&g_config_lock);
    return 0;
}

const char * get_listen_ip(void)
{
    return g_config->listen_ip;
}

int get_listen_port(void)
{
    return g_config->listen_port;
}

int get_worker_num(void)
{
    return g_config->worker_num;
}

app_log_level_t get_log_level(void)
{
    return g_config->log_level;
}

const char * get_log_file(void)
{
    return g_config->log_file;

}

void free_config(void)
{
    pthread_rwlock_wrlock(&g_config_lock);
    free(g_config);
    g_config = NULL;
    pthread_rwlock_unlock(&g_config_lock);
}
int get_config_snapshot(server_config_t *out)
{
    if (out == NULL) {
        return -1;
    }

    pthread_rwlock_rdlock(&g_config_lock);

    if (g_config == NULL) {
        pthread_rwlock_unlock(&g_config_lock);
        return -1;
    }

    /*
     * 如果 server_config_t 里面都是 int、char 数组这种固定字段，
     * 这里直接结构体拷贝是安全的。
     */
    *out = *g_config;

    pthread_rwlock_unlock(&g_config_lock);

    return 0;
}