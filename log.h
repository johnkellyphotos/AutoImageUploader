#include <sys/stat.h>

void _log(const char *fmt, ...) 
{
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
