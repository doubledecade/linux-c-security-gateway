//
// Created by iFOURYTWOLF on 26-6-7.
//
#define _POSIX_C_SOURCE 200809L
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>

static FILE *g_log_fp = NULL;
static app_log_level_t g_log_level = APP_LOG_INFO;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *level_to_string(app_log_level_t level)
{
    switch (level) {
        case APP_LOG_DEBUG:
            return "DEBUG";
        case APP_LOG_INFO:
            return "INFO";
        case APP_LOG_WARN:
            return "WARN";
        case APP_LOG_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

int app_log_init(const char *log_path, app_log_level_t level)
{
    if (log_path == NULL) {
        return -1;
    }

    pthread_mutex_lock(&g_log_mutex);

    if (g_log_fp != NULL) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }

    g_log_fp = fopen(log_path, "a");
    if (g_log_fp == NULL) {
        pthread_mutex_unlock(&g_log_mutex);
        return -1;
    }

    g_log_level = level;

    /*
     * 设置为行缓冲。
     * 每写一行日志，尽量及时刷到文件。
     */
    setvbuf(g_log_fp, NULL, _IOLBF, 0);

    pthread_mutex_unlock(&g_log_mutex);

    return 0;
}

void app_log_close(void)
{
    pthread_mutex_lock(&g_log_mutex);

    if (g_log_fp != NULL) {
        fflush(g_log_fp);
        fclose(g_log_fp);
        g_log_fp = NULL;
    }

    pthread_mutex_unlock(&g_log_mutex);
}

void app_log_write(app_log_level_t level,
                   const char *file,
                   int line,
                   const char *fmt, ...)
{
    time_t now;
    struct tm tm_now;
    char time_buf[64];
    va_list ap;

    if (level < g_log_level) {
        return;
    }

    pthread_mutex_lock(&g_log_mutex);

    if (g_log_fp == NULL) {
        pthread_mutex_unlock(&g_log_mutex);
        return;
    }

    now = time(NULL);
    localtime_r(&now, &tm_now);

    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_now);

    fprintf(g_log_fp,
            "[%s] [%s] [pid:%d] [tid:%lu] [%s:%d] ",
            time_buf,
            level_to_string(level),
            getpid(),
            (unsigned long)pthread_self(),
            file,
            line);

    va_start(ap, fmt);
    vfprintf(g_log_fp, fmt, ap);
    va_end(ap);

    fprintf(g_log_fp, "\n");
    fflush(g_log_fp);

    pthread_mutex_unlock(&g_log_mutex);
}