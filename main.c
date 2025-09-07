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

// compile with: `gcc main.c -o uploader -lcurl -ljson-c`

void _log(const char *note) 
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char buf[64];

    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", t);

    FILE *f = fopen("log.txt", "a");

    if (f) {
        fprintf(f, "[%s] %s\n", buf, note);
        fclose(f);
    }
}

void _logf(const char *fmt, ...) 
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);

    char buf[512];
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    _log(buf);
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
        _logf("Unable to log upload in track file (%s) for %s.", TRACK_FILE, filename);
        return;
    }
    fprintf(f, "%s\n", filename);
    fclose(f);
    _logf("Tracked upload for %s in track file.", filename);
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
            _logf("Failed to open file: %s.", filepath);
            curl_easy_cleanup(curl);
            return 0;
        }

        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_READDATA, hd_src);

        if (curl_easy_perform(curl) == CURLE_OK)
        {
            _logf("FTP of file complete for image %s to %s.", filepath, FTP_URL);
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
            _logf("Getting all files from device at %s...", fullpath);

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
            _logf("Detected camera: %s", camera_name);
            break;
        }
    }

    pclose(fp);
    return found;
}

int main(int argc, char *argv[]) 
{
    _log("Program start.");
    load_config();
    _log("Configuration complete.");

    if (argc > 1 && strcmp(argv[1], "--reset") == 0) 
    {
        _log("Attempting to clear track file...");
        FILE *f = fopen(TRACK_FILE, "w");
        if (f)
        {
            fclose(f); // truncate file to empty
            _log("Track file cleared.");
        }
        else
        {
            _log("Failed to clear track file.");
        }
    }

    pid_t gphoto_pid = -1;

    while (1) 
    {
        if (gphoto_pid <= 0) 
        {
            if (!camera_present()) 
            {
                _log("No camera detected. Waiting 2s before retry.");
                sleep(2);
                continue;
            }

            download_existing_files();

            gphoto_pid = fork();
            if (gphoto_pid == 0) 
            {
                char filename[1100];
                snprintf(filename, sizeof(filename), "%s/%%f_%%Y%%m%%d-%%H%%M%%S_%%C.jpg", LOCAL_DIR);

                _logf("Saving file from camera to to %s.", filename);
                execlp("gphoto2", "gphoto2", "--wait-event-and-download", "--skip-existing", "--folder", "/", "--filename", filename, NULL); // starts child process to download image

                _logf("Failed to save file from camera to to %s.", filename);
                _exit(1);
            }
            else if (gphoto_pid < 0) 
            {
                _log("Failed to fork process. continuing after 2 second wait...");
                perror("fork failed for wait-event-and-download");
                sleep(2);
                continue;
            }
        }

        int status;
        pid_t ret = waitpid(gphoto_pid, &status, WNOHANG);
        if (ret > 0) 
        {
            if (WIFEXITED(status)) 
            {
                int code = WEXITSTATUS(status);
                if (code == 0) 
                {
                    _log("Task completed and exited cleanly (process done).");
                }
                 else if (code == 1) 
                 {
                    _log("Task completed (likely no more events / camera removed).");
                }
                else
                {
                    _logf("gphoto2 exited with error code %d (likely camera disconnect).", code);
                }
            } 
            else if (WIFSIGNALED(status)) 
            {
                int sig = WTERMSIG(status);
                _logf("gphoto2 terminated by signal %d (likely camera disconnect).", sig);
            }

            gphoto_pid = -1;
            sleep(2);
        }

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
                
                if (!ext || (strcmp(ext, ".jpg") != 0 && strcmp(ext, ".JPG") != 0))
                {
                    _logf("Skipping upload of non JPEG image: %s because it is extension type: %s.", dir->d_name, ext ? ext : "(none)");
                    continue;
                }

                if (is_uploaded(dir->d_name))
                {
                    _logf("Skipping upload of %s because it marked as uploaded in track file.", dir->d_name);
                    continue;
                }

                char path[1024];
                snprintf(path, sizeof(path), "%s/%s", LOCAL_DIR, dir->d_name);

                _logf("Attempting to upload file: %s.", dir->d_name);
                if (upload_file(path, dir->d_name))
                {
                    _log("Upload complete.");
                    mark_uploaded(dir->d_name);
                }
                else
                {
                    _logf("Upload failed for file: %s.", dir->d_name);
                }
            }

            closedir(d);
        }
        else
        {
            _logf("Failed to open directory %s.", LOCAL_DIR);
        }

        sleep(2);
    }

    return 0;
}