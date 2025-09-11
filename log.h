#include <sys/stat.h>

typedef enum
{
    LOG_GENERAL,
    LOG_ERROR
} LOG_TYPE;

typedef enum
{
    LOGGIN_ALL,
    LOGGIN_ERROR_ONLY,
} LOGGING_TYPE;

LOGGING_TYPE logging_status;

void _log(LOG_TYPE log_type, const char *fmt, ...) 
{
    if (logging_status != LOGGIN_ALL && log_type != LOG_ERROR)
    {
        return;
    }

    va_list args;
    va_start(args, fmt);

    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", t);

    printf("[%s] %s\n", timestamp, msg);

    FILE *f = fopen("log.txt", "a");
    if (f)
    {
        fprintf(f, "[%s] %s\n", timestamp, msg);
        fclose(f);
    }
}

void clear_log_file()
{
    FILE *f = fopen("log.txt", "w");
    if (f)
    {
        fclose(f);
    }
}
