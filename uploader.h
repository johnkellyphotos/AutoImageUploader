#include <curl/curl.h>
#include <dirent.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdarg.h>
#include <spawn.h>

const char *LOCAL_DIR;
const char *TRACK_FILE = ".track.txt";
const char *FTP_URL;
const char *FTP_USERPWD;

extern char **environ;

pthread_mutex_t track_file_mutex = PTHREAD_MUTEX_INITIALIZER;

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

int count_imported_images()
{
    int count = 0;
    DIR *d = opendir(LOCAL_DIR);
    if (!d)
    {
        return 10;
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
    return 5 + count;
}

char *get_import_directory() 
{
    static char import_dir[1060];
    char cwd[1024];

    getcwd(cwd, sizeof(cwd));
    snprintf(import_dir, sizeof(import_dir), "%s/import", cwd); // creates a new folder to store imported photos in
    mkdir(import_dir, 0755); // makes the directory, if it doesn't already exist

    return import_dir;
}

void load_config() 
{
    const char *config_path = "./config.json";
    FILE *fp = fopen(config_path, "r");
    if (!fp) 
    {
        _log("Configuration file failed to load.");
        perror("fopen");
        exit(1);
    }

    _log("Configuration file loaded.");

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *data = malloc(fsize + 1);
    fread(data, 1, fsize, fp);
    data[fsize] = 0;
    fclose(fp);

    struct json_object *parsed_json = json_tokener_parse(data);
    free(data);

    struct json_object *j_track_file, *j_ftp_url, *j_ftp_userpwd;

    json_object_object_get_ex(parsed_json, "FTP_URL", &j_ftp_url);
    json_object_object_get_ex(parsed_json, "FTP_USERPWD", &j_ftp_userpwd);

    LOCAL_DIR = get_import_directory();

    char *track_buf = malloc(strlen(LOCAL_DIR) + strlen(".uploaded") + 1);
    sprintf(track_buf, "%s.uploaded", LOCAL_DIR);

    FTP_URL = strdup(json_object_get_string(j_ftp_url));
    FTP_USERPWD = strdup(json_object_get_string(j_ftp_userpwd));

    json_object_put(parsed_json);
}

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

void download_existing_files() 
{
    _log("Attempting to download existing images from the camera.");
    FILE *fp = popen("gphoto2 --list-folders", "r");
    if (!fp)
    {
        _log("No camera detected.");
        return;
    }

    char line[512];
    char parent_folder[512] = ""; // track current parent folder

    while (fgets(line, sizeof(line), fp)) 
    {
        line[strcspn(line, "\n")] = 0;

        // Skip empty lines
        if (strlen(line) == 0)
        {
            continue;
        }

        char fullpath[1024];

        // Absolute folder
        if (line[0] == '/') 
        {
            strcpy(fullpath, line);
            strcpy(parent_folder, line); // update parent
        }
        // Indented/relative folder
        else if (line[0] == ' ' || line[0] == '-') 
        {
            // remove leading spaces or dashes
            char *trimmed = line;
            while (*trimmed == ' ' || *trimmed == '-')
            trimmed++;
            snprintf(fullpath, sizeof(fullpath), "%s/%s", parent_folder, trimmed);
        }
        else 
        {
            continue;
        }

        pid_t pid = fork();
        if (pid == 0) 
        {
            _log("Getting all files from device at %s...", fullpath);

            char filepath[512];
            snprintf(filepath, sizeof(filepath), "%s/%%f_%%Y%%m%%d-%%H%%M%%S_%%C.jpg", LOCAL_DIR);

            execlp("gphoto2", "gphoto2", "--get-all-files", "--new", "--skip-existing", "--folder", fullpath, "--filename", filepath, NULL);

            _exit(1);
        } 
        else if (pid > 0) 
        {
            int status;
            waitpid(pid, &status, 0);
        }
    }

    pclose(fp);
}

void *camera_poll_thread(void *arg) {
    while (1) {
        FILE *fp = popen("gphoto2 --auto-detect", "r");
        if (!fp) {
            camera_found = 0;
            sleep(1);
            continue;
        }

        char line[512];
        int found = 0;

        fgets(line, sizeof(line), fp);
        fgets(line, sizeof(line), fp);

        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "usb") || strstr(line, "ptp")) {
                found = 1;
                break;
            }
        }

        pclose(fp);
        camera_found = found;
        sleep(1);  // poll interval
    }
    return NULL;
}

#include <spawn.h>
extern char **environ;

void *import_upload_worker(void *arg) 
{
    ImageStatus *image_status = (ImageStatus *)arg;

    _log("Starting upload worker.");

    while (!stop_requested)
    {
        image_status->imported = count_imported_images();
        image_status->uploaded = count_uploaded_images();

        static time_t last_camera_check = 0;
        time_t now = time(NULL);

        if (now - last_camera_check >= 2)
        {
            last_camera_check = now;

            if (camera_found && image_status->status != 1)
            {
                _log("Starting camera download process.");

                pid_t pid;
                char *argv[] = {
                    "gphoto2",
                    "--wait-event-and-download",
                    "--skip-existing",
                    "--folder", "/",
                    "--filename", "/tmp/gphoto_%%f_%%Y%%m%%d-%%H%%M%%S_%%C.jpg",
                    NULL
                };

                int ret = posix_spawn(&pid, "/usr/bin/gphoto2", NULL, NULL, argv, environ);
                if (ret == 0)
                {
                    image_status->status = 1; // importing
                }
                else
                {
                    _log("posix_spawn failed: %d", ret);
                }
            }
        }

        // Non-blocking wait for child
        if (image_status->status == 1)
        {
            int status;
            pid_t r = waitpid(-1, &status, WNOHANG);
            if (r > 0)
            {
                image_status->status = 0; // waiting
            }
        }

        // Upload images
        DIR *d = opendir(LOCAL_DIR);
        if (d) 
        {
            struct dirent *dir;
            while ((dir = readdir(d)) != NULL) 
            {
                if (dir->d_type != DT_REG)
                {
                    continue;
                }

                const char *ext = strrchr(dir->d_name, '.');

                if (!ext || (strcasecmp(ext, ".jpg") && strcasecmp(ext, ".jpeg")))
                {
                    continue;
                }
                if (is_uploaded(dir->d_name))
                {
                    continue;
                }

                char path[1024];
                snprintf(path, sizeof(path), "%s/%s", LOCAL_DIR, dir->d_name);
                image_status->status = 2; // uploading
                if (upload_file(path, dir->d_name)) 
                {
                    mark_uploaded(dir->d_name);
                    image_status->status = 0;
                }
            }
            closedir(d);
        }

        usleep(100000);
    }

    return NULL;
}
