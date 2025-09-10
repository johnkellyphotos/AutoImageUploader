#include <curl/curl.h>
#include <sys/stat.h>

pthread_mutex_t track_file_mutex = PTHREAD_MUTEX_INITIALIZER;

int is_uploaded(const char *filename) 
{
    pthread_mutex_lock(&track_file_mutex);

    FILE *f = fopen(TRACK_FILE, "r");
    if (!f) 
    {
        pthread_mutex_unlock(&track_file_mutex);
        return 0;
    }

    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f)) 
    {
        line[strcspn(line, "\n")] = 0;
        if (strcmp(line, filename) == 0) 
        {
            found = 1;
            break;
        }
    }

    fclose(f);
    pthread_mutex_unlock(&track_file_mutex);
    return found;
}

int upload_file(const char *filepath, const char *filename) 
{
    CURL *curl = curl_easy_init();
    int success = 0;
    if (curl) 
    {
        char url[1024];
        snprintf(url, sizeof(url), "%s%s", FTP_URL, filename);
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_USERPWD, FTP_USERPWD);
        FILE *hd_src = fopen(filepath, "rb");

        if (!hd_src) 
        {
            _log("Failed to open file: %s.", filepath);
            curl_easy_cleanup(curl);
            return 0;
        }

        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_READDATA, hd_src);

        if (curl_easy_perform(curl) == CURLE_OK)
        {
            _log("FTP of file complete for image %s to %s.", filepath, FTP_URL);
            success = 1;
        }

        fclose(hd_src);
        curl_easy_cleanup(curl);
    }
    else
    {
        _log("CURL failed to initialize.");
    }

    return success;
}

void mark_uploaded(const char *filename) 
{
    pthread_mutex_lock(&track_file_mutex);

    FILE *f = fopen(TRACK_FILE, "a");
    if (f) 
    {
        fprintf(f, "%s\n", filename);
        fclose(f);
        _log("Tracked upload for %s in track file.", filename);
    } 
    else 
    {
        _log("Unable to log upload in track file (%s) for %s.", TRACK_FILE, filename);
    }

    pthread_mutex_unlock(&track_file_mutex);
}

int count_imported_images()
{
    int count = 0;
    DIR *d = opendir(LOCAL_DIR);
    if (!d)
    {
        return 0;
    }

    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) 
    {
        if (dir->d_type != DT_REG)
        {
            continue;
        }
        const char *ext = strrchr(dir->d_name, '.');
        if (!ext)
        {
            continue;
        }
        if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg"))
        {
            count++;
        }
    }

    closedir(d);
    return count;
}

int count_uploaded_images()
{
    FILE *f = fopen(TRACK_FILE, "r");
    if (!f)
    {
        return 0;
    }

    int count = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) 
    {
        line[strcspn(line, "\n")] = 0;
        if (strlen(line) > 0)
        {
            count++;
        }
    }

    fclose(f);
    return count;
}
