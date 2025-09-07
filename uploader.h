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

const char *LOCAL_DIR;
const char *TRACK_FILE = ".track.txt";
const char *FTP_URL;
const char *FTP_USERPWD;

void _log(const char *fmt, ...) 
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);

    char buf[512];
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", t);

    FILE *f = fopen("log.txt", "a");

    if (f)
    {
        fprintf(f, "[%s] %s\n", buf, fmt);
        fclose(f);
    }
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
    FILE *f = fopen(TRACK_FILE, "r");
    if (!f)
    {
        return 0;
    }

    char line[512];
    
    while (fgets(line, sizeof(line), f)) 
    {
        line[strcspn(line, "\n")] = 0;
        if (strcmp(line, filename) == 0) 
        {
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

void mark_uploaded(const char *filename) 
{
    FILE *f = fopen(TRACK_FILE, "a");
    if (!f)
    {
        _log("Unable to log upload in track file (%s) for %s.", TRACK_FILE, filename);
        return;
    }
    fprintf(f, "%s\n", filename);
    fclose(f);
    _log("Tracked upload for %s in track file.", filename);
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

int camera_present() 
{
    FILE *fp = popen("gphoto2 --auto-detect", "r");
    if (!fp) return 0;

    char line[512];
    int found = 0;
    char camera_name[256] = {0};

    // skip header lines
    fgets(line, sizeof(line), fp);
    fgets(line, sizeof(line), fp);

    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "usb") || strstr(line, "ptp")) {
            found = 1;

            char *last_space = strrchr(line, '\t');
            if (!last_space) last_space = strrchr(line, ' ');
            if (last_space) *last_space = 0;

            strncpy(camera_name, line, sizeof(camera_name) - 1);
            _log("Detected camera: %s", camera_name);
            break;
        }
    }

    pclose(fp);
    return found;
}
