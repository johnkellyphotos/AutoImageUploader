#include <dirent.h>
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
#include <sys/stat.h>

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
        kill_camera_users(); // release any existing handles before retrying

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
            _log("Camera device busy (-53), will retry.");
            camera_found = -1;
        }
        else
        {
            _log("Camera init failed (ret=%d: %s)", ret, gp_result_as_string(ret));
            camera_found = 0;
        }

        gp_camera_exit(global_camera, global_context);
        gp_camera_free(global_camera);
        global_camera = NULL;
        sleep(1);
    }

    camera_initialized = 0;
    camera_found = 0;
    _log("Camera could not be initialized.");
}

void download_existing_files_from_camera()
{
    pthread_mutex_lock(&camera_mutex);

    if (!camera_initialized)
    {
        camera_init_global();
        if (!camera_initialized)
        {
            _log("Camera failed intialized in download_existing_files_from_camera()... aborting...");
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

int check_if_camera_is_connected()
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
        download_existing_files_from_camera();
        _log("Existing file download complete.");
        image_status->imported = count_imported_images();
        image_status->uploaded = count_uploaded_images();

        static time_t last_camera_check = 0;
        time_t now = time(NULL);


        // check if camera connected
        camera_found = check_if_camera_is_connected();
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
            image_status->status = 2; // uploading
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
                if (upload_file(path, dir->d_name)) 
                {
                    mark_uploaded(dir->d_name);
                }
            }
            image_status->status = 0;
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