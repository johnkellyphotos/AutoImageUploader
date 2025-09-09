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

pthread_mutex_t track_file_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t camera_mutex = PTHREAD_MUTEX_INITIALIZER;

Camera *global_camera = NULL;
GPContext *global_context = NULL;

volatile int camera_initialized = 0;
volatile int camera_busy_flag = 0;

void kill_camera_users() 
{
    libusb_context *ctx;
    libusb_device **list;
    ssize_t cnt;
    libusb_init(&ctx);
    cnt = libusb_get_device_list(ctx, &list);

    for (ssize_t i = 0; i < cnt; i++)
    {
        struct libusb_device_descriptor desc;
        libusb_get_device_descriptor(list[i], &desc);
        if (desc.idVendor == 0x04b0 && desc.idProduct == 0x043a)
        {
            uint8_t bus = libusb_get_bus_number(list[i]);
            uint8_t addr = libusb_get_device_address(list[i]);
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "fuser -k /dev/bus/usb/%03d/%03d", bus, addr);
            system(cmd);
        }
    }

    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
}

void handle_sigint(int sig)
{
    _log("Logging signal interrupt: %i", sig);
    stop_requested = 1;
    kill_camera_users();
}

void list_files_recursive(const char *folder)
{
    _log("Recursively entering folder: %s", folder);

    CameraList *subfolders = NULL;
    gp_list_new(&subfolders);
    
    int ret = gp_camera_folder_list_folders(global_camera, folder, subfolders, global_context);
    if (ret >= GP_OK)
    {
        int sub_count = gp_list_count(subfolders);
        for (int i = 0; i < sub_count; i++)
        {
            const char *sub = NULL;
            gp_list_get_name(subfolders, i, &sub);
            if (sub)
            {
                char path[1024];
                if (strcmp(folder, "/") == 0)
                {
                    snprintf(path, sizeof(path), "/%s", sub);
                }
                else
                {
                    snprintf(path, sizeof(path), "%s/%s", folder, sub);
                }
                list_files_recursive(path);
            }
        }
    }
    gp_list_free(subfolders);

    CameraList *files = NULL;
    gp_list_new(&files);
    ret = gp_camera_folder_list_files(global_camera, folder, files, global_context);
    if (ret >= GP_OK)
    {
        int file_count = gp_list_count(files);
        for (int j = 0; j < file_count; j++)
        {
            const char *filename = NULL;
            gp_list_get_name(files, j, &filename);
            if (filename)
            {
                _log("Downloading file %s/%s", folder, filename);

                CameraFile *file = NULL;
                gp_file_new(&file);

                if (gp_camera_file_get(global_camera, folder, filename, GP_FILE_TYPE_NORMAL, file, global_context) >= GP_OK)
                {
                    char file_path[8192];
                    snprintf(file_path, sizeof(file_path), "%s/%s", LOCAL_DIR, filename);

                    struct stat st;
                    if (stat(file_path, &st) == 0)
                    {
                        _log("Skipping existing file %s", file_path);
                    }
                    else
                    {
                        gp_file_save(file, file_path);
                        _log("Saved file to %s", file_path);
                    }
                }
                else
                {
                    _log("Failed to download file %s/%s", folder, filename);
                }
                gp_file_free(file);
            }
        }
    }
    gp_list_free(files);
}

void camera_init_global()
{
    if (camera_initialized)
    {
        _log("Camera already inited");
         return;
    }

    _log("Attempting global init of camera.");
    global_context = gp_context_new();
    if (!global_context)
    {
        camera_found = 0;
        _log("Failed to create gphoto2 context.");
        return;
    }

    const int MAX_RETRIES = 3;
    for (int attempt = 0; attempt < MAX_RETRIES; attempt++)
    {
        if (gp_camera_new(&global_camera) < GP_OK)
        {
            _log("Failed to create camera object (attempt %d)", attempt + 1);
            sleep(1);
            continue;
        }

        int ret = gp_camera_init(global_camera, global_context);
        if (ret >= GP_OK)
        {
            camera_initialized = 1;
            camera_found = 1;
            _log("Camera initialized.");
            return;
        }
        else if (ret == -53)
        {
            camera_found = -1;
        }
        else
        {
            camera_found = 0;
        }

        _log("Camera init failed (ret=%d: %s), retrying...", ret, gp_result_as_string(ret));
        gp_camera_free(global_camera);
        global_camera = NULL;
        sleep(1);
    }

    camera_initialized = 0;
    camera_found = 0;
    _log("Camera could not be initialized.");
}

void download_existing_files()
{
    pthread_mutex_lock(&camera_mutex);

    if (!camera_initialized)
    {
        camera_init_global();
        if (!camera_initialized)
        {
            _log("Camera failed intialized in download_existing_files()... aborting...");
            pthread_mutex_unlock(&camera_mutex);
            return;
        }
    }

    camera_busy_flag = 1;
    CameraList *folders = NULL;
    gp_list_new(&folders);
    int ret = gp_camera_folder_list_folders(global_camera, "/", folders, global_context);
    if (ret < GP_OK)
    {
        _log("Failed to list folders: %d", ret);
        gp_list_free(folders);
        camera_busy_flag = 0;
        pthread_mutex_unlock(&camera_mutex);
        return;
    }

    list_files_recursive("/");

    gp_list_free(folders);
    camera_busy_flag = 0;
    pthread_mutex_unlock(&camera_mutex);
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

    struct json_object *j_ftp_url, *j_ftp_userpwd;

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

int check_camera_connected()
{
    int found = 0;

    pthread_mutex_lock(&camera_mutex);

    gp_camera_exit(global_camera, global_context); // close previous session
    int ret = gp_camera_init(global_camera, global_context);
    found = (ret >= GP_OK) ? 1 : 0;

    pthread_mutex_unlock(&camera_mutex);

    return found;
}

void *import_upload_worker(void *arg) 
{
    ImageStatus *image_status = (ImageStatus *)arg;

    _log("Starting upload worker.");

    while (!stop_requested) 
    {
        download_existing_files();
        _log("Existing file download complete.");
        image_status->imported = count_imported_images();
        image_status->uploaded = count_uploaded_images();

        static time_t last_camera_check = 0;
        time_t now = time(NULL);


        // check if camera connected
        camera_found = check_camera_connected();
        _log("Camera status check complete: %i", camera_found);

        if (now - last_camera_check >= 2) 
        {
            _log("Entering...");
            last_camera_check = now;

            if ((camera_found > 0)) 
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
            else
            {
                _log("Camera not found in import...");
            }
        }

        // Upload images
        DIR *d = opendir(LOCAL_DIR);
        if (internet_up && d) 
        {
            _log("FTP beginning...");
            struct dirent *dir;
            int counter = 0;
            while ((dir = readdir(d)) != NULL) 
            {
                counter++;
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

                _log("Ready to try FTP");

                char path[1024];
                snprintf(path, sizeof(path), "%s/%s", LOCAL_DIR, dir->d_name);
                image_status->status = 2; // uploading
                if (upload_file(path, dir->d_name)) 
                {
                    _log("case 5");
                    mark_uploaded(dir->d_name);
                    image_status->status = 0;
                }
            }
            closedir(d);
        } 
        else if (!internet_up && (camera_found > 0)) 
        {
            _log("No FTP attempt...");
            image_status->status = 3;
        }

        usleep(100000);
    }

    return NULL;
}