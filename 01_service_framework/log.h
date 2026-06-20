//
// Created by iFOURYTWOLF on 26-6-7.
//

#ifndef LINUX_C_SECURITY_GATEWAY_LOG_H
#define LINUX_C_SECURITY_GATEWAY_LOG_H
typedef enum {
    APP_LOG_DEBUG = 0,
    APP_LOG_INFO,
    APP_LOG_WARN,
    APP_LOG_ERROR
} app_log_level_t;

int app_log_init(const char *log_path, app_log_level_t level);
void app_log_close(void);

void app_log_write(app_log_level_t level,
                   const char *file,
                   int line,
                   const char *fmt, ...);

#define LOG_DEBUG(fmt, ...) \
    app_log_write(APP_LOG_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_INFO(fmt, ...) \
    app_log_write(APP_LOG_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_WARN(fmt, ...) \
    app_log_write(APP_LOG_WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_ERROR(fmt, ...) \
    app_log_write(APP_LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#endif //LINUX_C_SECURITY_GATEWAY_LOG_H
