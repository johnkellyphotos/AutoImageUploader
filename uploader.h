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
#include <gphoto2/gphoto2-camera.h>
#include <gphoto2/gphoto2-context.h>

const char *LOCAL_DIR;
const char *TRACK_FILE = ".track.txt";
const char *FTP_URL;
const char *FTP_USERPWD;

extern char **environ;

pthread_mutex_t track_file_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t camera_mutex = PTHREAD_MUTEX_INITIALIZER;

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
    snprintf(import_dir, sizeof(import_dir), "%s/import", cwd);

    struct stat st;
    if (stat(import_dir, &st) != 0 || !S_ISDIR(st.st_mode)) 
    {
        mkdir(import_dir, 0755);
    }

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

    pthread_mutex_lock(&camera_mutex);
    Camera *camera;
    GPContext *context = gp_context_new();
    int ret = gp_camera_new(&camera);
    if (ret < GP_OK)
    {
        _log("Failed to create camera object: %d", ret);
        pthread_mutex_unlock(&camera_mutex);
        return;
    }

    ret = gp_camera_init(camera, context);
    if (ret < GP_OK)
    {
        if (ret == GP_ERROR_MODEL_NOT_FOUND)
        {
            camera_found = -1;
            _log("Camera detected but communication with camera failed.");
        }
        else if (ret == -53)
        {
            camera_found = -1;
            _log("Camera detected but the camera is busy.");
        }
        else
        {
            camera_found = 0;
            _log("No camera detected or failed to initialize: %d", ret);
            gp_camera_exit(camera, context);
            pthread_mutex_unlock(&camera_mutex);
            return;
        }
    }
    else
    {
        CameraList *folders;
        gp_list_new(&folders);

        ret = gp_camera_folder_list_folders(camera, "/", folders, context);

        if (ret < GP_OK)
        {
            _log("Failed to list folders: %d", ret);
            gp_list_free(folders);
            gp_camera_exit(camera, context);
            pthread_mutex_unlock(&camera_mutex);
            return;
        }

        int folder_count = gp_list_count(folders);
        for (int i = 0; i < folder_count; i++)
        {
            const char *folder;
            gp_list_get_name(folders, i, &folder);
            _log("Processing folder: %s", folder);

            CameraList *files;
            gp_list_new(&files);
            ret = gp_camera_folder_list_files(camera, folder, files, context);
            if (ret < GP_OK)
            {
                _log("Failed to list files in folder %s: %d", folder, ret);
                gp_list_free(files);
                continue;
            }

            int file_count = gp_list_count(files);
            for (int j = 0; j < file_count; j++)
            {
                const char *filename;
                gp_list_get_name(files, j, &filename);
                _log("Downloading file %s/%s", folder, filename);

                CameraFile *file;
                gp_file_new(&file);
                ret = gp_camera_file_get(camera, folder, filename, GP_FILE_TYPE_NORMAL, file, context);
                if (ret < GP_OK)
                {
                    _log("Failed to download %s/%s: %d", folder, filename, ret);
                    gp_file_free(file);
                    continue;
                }

                char path[1024];
                snprintf(path, sizeof(path), "%s/%s", LOCAL_DIR, filename);
                ret = gp_file_save(file, path);
                if (ret < GP_OK)
                {
                    _log("Failed to save %s: %d", path, ret);
                }
                else
                {
                    _log("Saved %s", path);
                }

                gp_file_free(file);
            }

            gp_list_free(files);
        }
            gp_list_free(folders);
            gp_camera_exit(camera, context);
    }
    
    pthread_mutex_unlock(&camera_mutex);
}

void *camera_poll_thread(void *arg) 
{
    Camera *camera = NULL;
    GPContext *context = gp_context_new();
    int camera_initialized = 0;

    while (!stop_requested) 
    {
        pthread_mutex_lock(&camera_mutex);
        if (!camera_initialized) 
        {
            if (gp_camera_new(&camera) >= GP_OK && gp_camera_init(camera, context) >= GP_OK) 
            {
                camera_found = 1;
                camera_initialized = 1;
            } 
            else 
            {
                camera_found = 0;
                if (camera)
                {
                    gp_camera_free(camera); camera = NULL;
                }
            }
        } 
        else 
        {
            // Camera already initialized; optionally poll events
            CameraEventType event_type;
            void *event_data = NULL;
            int ret = gp_camera_wait_for_event(camera, 2000, &event_type, &event_data, context);

            if (ret != GP_OK) 
            {
                camera_found = 0;
                gp_camera_exit(camera, context);
                gp_camera_free(camera);
                camera = NULL;
                camera_initialized = 0;
            } 
            else 
            {
                camera_found = 1;
            }
        }

        pthread_mutex_unlock(&camera_mutex);

        sleep(1);
    }
}

void *import_upload_worker(void *arg) 
{
    ImageStatus *image_status = (ImageStatus *)arg;

    _log("Starting upload worker.");

    while (!stop_requested) 
    {
        download_existing_files();
        image_status->imported = count_imported_images();
        image_status->uploaded = count_uploaded_images();

        static time_t last_camera_check = 0;
        time_t now = time(NULL);

        if (now - last_camera_check >= 2) 
        {
            last_camera_check = now;

            if ((camera_found > 0) && image_status->status != 1) 
            {
                pthread_mutex_lock(&camera_mutex);
                _log("Starting camera download process.");

                Camera *camera;
                GPContext *context = gp_context_new();
                int ret = gp_camera_new(&camera);
                if (ret < GP_OK) 
                {
                    _log("Failed to create camera instance.");
                } 
                else 
                {
                    ret = gp_camera_init(camera, context);
                    if (ret < GP_OK) 
                    {
                        _log("Failed to initialize camera.");
                        gp_camera_free(camera);
                    } 
                    else 
                    {
                        CameraFile *file;
                        gp_file_new(&file);

                        CameraEventType event_type;
                        void *event_data = NULL;
                        ret = gp_camera_wait_for_event(camera, 2000, &event_type, &event_data, context);

                        if (event_type == GP_EVENT_FILE_ADDED) 
                        {
                            CameraFilePath *path = (CameraFilePath *)event_data;
                            _log("New file added: %s/%s", path->folder, path->name);
                        }

                        if (ret == GP_OK) 
                        {
                            image_status->status = internet_up ? 1 : 3;
                        } 
                        else 
                        {
                            _log("Camera event wait failed: %d", ret);
                        }

                        gp_file_free(file);
                        gp_camera_exit(camera, context);
                        gp_camera_free(camera);
                    }
                }
                pthread_mutex_unlock(&camera_mutex);
            }
        }

        // Upload images
        DIR *d = opendir(LOCAL_DIR);
        if (internet_up && d) 
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
        else if (!internet_up && (camera_found > 0)) 
        {
            image_status->status = 3;
        }

        usleep(100000);
    }

    return NULL;
}