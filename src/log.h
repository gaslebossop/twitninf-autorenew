#ifndef ARW_LOG_H
#define ARW_LOG_H

typedef enum {
    ARW_LOG_DEBUG,
    ARW_LOG_INFO,
    ARW_LOG_WARN,
    ARW_LOG_ERROR
} arw_log_level;

void arw_log_set_level(arw_log_level level);
void arw_log(arw_log_level level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define LOG_DEBUG(...) arw_log(ARW_LOG_DEBUG, __VA_ARGS__)
#define LOG_INFO(...)  arw_log(ARW_LOG_INFO,  __VA_ARGS__)
#define LOG_WARN(...)  arw_log(ARW_LOG_WARN,  __VA_ARGS__)
#define LOG_ERROR(...) arw_log(ARW_LOG_ERROR, __VA_ARGS__)

#endif /* ARW_LOG_H */
