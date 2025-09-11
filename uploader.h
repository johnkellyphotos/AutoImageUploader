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
#include <glib.h>

pthread_mutex_t camera_mutex = PTHREAD_MUTEX_INITIALIZER;
GHashTable *downloaded_files = NULL;

Camera *global_camera = NULL;
GPContext *global_context = NULL;

volatile int camera_initialized = 0;
volatile int camera_busy_flag = 0;

void kill_device_mount_to_camera()
{
    /* 
    * In some occasions, the filesystem will mount the device BEFORE this program can mount it.
    * By killing all mounts for the camera device, we allow this program to retry
    * to mount, where the filesystem will not attempt to remount under normal conditions. If
    * this were to fail, the user would need to manually unmount the camera for the filesystem
    */
    system("pkill -f gvfsd-gphoto2");
    system("pkill -f gvfs-gphoto2-volume-monitor");
}

static void camera_cleanup()
{
    if (global_camera)
    {
        gp_camera_exit(global_camera, global_context);
        gp_camera_free(global_camera);
        global_camera = NULL;
    }
    if (global_context)
    {
        gp_context_unref(global_context);
        global_context = NULL;
    }
    camera_initialized = camera_found = 0;
}

int fetch_file(const char *folder, const char *filename)
{
    CameraFile *file;
    gp_file_new(&file);

    int ret = gp_camera_file_get(global_camera, folder, filename, GP_FILE_TYPE_NORMAL, file, global_context);

    if (ret < GP_OK)
    {
        ret = gp_camera_file_get(global_camera, folder, filename, GP_FILE_TYPE_PREVIEW, file, global_context);
    }

    if (ret < GP_OK)
    {
        ret = gp_camera_file_get(global_camera, folder, filename, GP_FILE_TYPE_RAW, file, global_context);
    }

    if (ret >= GP_OK)
    {
        struct stat st;
        char file_path[8192];
        snprintf(file_path, sizeof(file_path), "%s/%s", LOCAL_DIR, filename);

        if (stat(file_path, &st) == 0)
        {
            _log(LOG_GENERAL, "Skipping existing file %s", file_path);
        }
        else
        {
            gp_file_save(file, file_path);
            _log(LOG_GENERAL, "Saved file to %s", file_path);
        }
    }
    else
    {
        _log(LOG_ERROR, "Failed to fetch file %s/%s (ret=%d: %s)", folder, filename, ret, gp_result_as_string(ret));
    }

    gp_file_free(file);
    return ret;
}

void list_files_recursive(const char *folder)
{
    if (!downloaded_files)
    {
        downloaded_files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    }

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
    else
    {
        camera_cleanup();
        camera_initialized = 0;
        camera_found = (ret == GP_ERROR_MODEL_NOT_FOUND) ? 0 : -1;
    }

    gp_list_free(subfolders);

    CameraList *files = NULL;
    gp_list_new(&files);

    ret = gp_camera_folder_list_files(global_camera, folder, files, global_context);
    if (ret >= GP_OK)
    {
        int file_count = gp_list_count(files);
        if (file_count > 0)
        {
            _log(LOG_GENERAL, "Importing images for folder %s", folder);
        }
        for (int j = 0; j < file_count; j++)
        {
            const char *filename = NULL;
            gp_list_get_name(files, j, &filename);
            if (filename)
            {
                char fullpath[2048];
                snprintf(fullpath, sizeof(fullpath), "%s/%s", folder, filename);

                if (!g_hash_table_contains(downloaded_files, fullpath))
                {
                    _log(LOG_GENERAL, "Downloading file %s", fullpath);
                    fetch_file(folder, filename);
                    g_hash_table_add(downloaded_files, g_strdup(fullpath));
                }
            }
        }
    }
    else
    {
        camera_cleanup();
        camera_initialized = 0;
        camera_found = (ret == GP_ERROR_MODEL_NOT_FOUND) ? 0 : -1;
    }
    gp_list_free(files);
}

static int try_init_camera_once()
{
    _log(LOG_GENERAL, "Attempting to initialize camera...");
    int ret;

    if (!global_context)
    {
        global_context = gp_context_new();
        if (!global_context)
        {
            return -1;
        }
    }

    if (global_camera)
    {
        gp_camera_exit(global_camera, global_context);
        gp_camera_free(global_camera);
        global_camera = NULL;
    }

    if ((ret = gp_camera_new(&global_camera)) < GP_OK)
    {
        camera_cleanup();
        return ret;
    }

    ret = gp_camera_init(global_camera, global_context);
    if (ret < GP_OK)
    {
        gp_camera_exit(global_camera, global_context);
        gp_camera_free(global_camera);
        global_camera = NULL;
        camera_found = -1;
        return ret;
    }

    _log(LOG_GENERAL, "Camera successfully initialized.");
    camera_initialized = 1;
    camera_found = 1;
    return GP_OK;
}

void camera_init_global(void)
{
    const int MAX_RETRIES = 2;
    int attempt = 0;

    camera_cleanup(); // ensure fresh state before attempting

    for (attempt = 0; attempt < MAX_RETRIES && !stop_requested; ++attempt)
    {
        int ret = try_init_camera_once();
        if (ret >= GP_OK)
        {
            _log(LOG_GENERAL, "Initialization attempt completed successfully.");
            return;
        }

        if (ret == GP_ERROR_MODEL_NOT_FOUND)
        {
            camera_found = 0;
            _log(LOG_GENERAL, "Camera not present (attempt %d).", attempt + 1);
        }
        else if (ret == GP_ERROR_CAMERA_BUSY || ret == -53)
        {
            kill_device_mount_to_camera();
            camera_found = -1;
            _log(LOG_ERROR, "Device busy (attempt %d). Attempting to kill existing mounts and", attempt + 1);
        }
        else
        {
            camera_found = 0;
            _log(LOG_ERROR, "gp init failed (ret=%d: %s) (attempt %d).", ret, gp_result_as_string(ret), attempt + 1);
        }

        sleep(1 + attempt); // backoff and stall before retrying
    }

    camera_initialized = 0;
    _log(LOG_GENERAL, "Camera could not be initialized.");
}

void download_existing_files_from_camera(ImageStatus *image_status)
{
    _log(LOG_GENERAL, "Attempting to download existing files to device from camera.");
    pthread_mutex_lock(&camera_mutex);

    if (!camera_initialized)
    {
        camera_init_global();
        if (!camera_initialized)
        {
            _log(LOG_ERROR, "Camera failed intialized in download_existing_files_from_camera()... aborting...");
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
        _log(LOG_ERROR, "Failed to list folders: %d", ret);
        gp_list_free(folders);
        camera_busy_flag = 0;
        pthread_mutex_unlock(&camera_mutex);
        return;
    }

    image_status->status = internet_up ? CAMERA_STATUS_IMPORTING : CAMERA_STATUS_IMPORT_ONLY;
    _log(LOG_GENERAL, "Importing images for from camera...");
    list_files_recursive("/");

    gp_list_free(folders);
    camera_busy_flag = 0;
    pthread_mutex_unlock(&camera_mutex);
}

void *import_upload_worker(void *arg) 
{
    ImageStatus *image_status = (ImageStatus *)arg;

    _log(LOG_GENERAL, "Starting upload worker.");

    while (!stop_requested) 
    {
        // Step 1: Ensure camera is initialized
        if (!camera_initialized)
        {
            pthread_mutex_lock(&camera_mutex);
            int ret = gp_camera_new(&global_camera);
            if (ret >= GP_OK)
            {
                ret = gp_camera_init(global_camera, global_context);
                if (ret >= GP_OK)
                {
                    camera_initialized = 1;
                    camera_found = 1;
                    _log(LOG_GENERAL, "Camera initialized in worker loop.");
                }
                else
                {
                    gp_camera_free(global_camera);
                    global_camera = NULL;
                    camera_initialized = 0;
                    camera_found = ret == -53 ? -1 : 0;
                    if (ret == -53 )
                    {
                        kill_device_mount_to_camera();
                    }
                    _log(LOG_GENERAL, ret == -53 ? "Camera busy... could not connect." :  "No camera detected.");
                }
            }
            else
            {
                camera_initialized = 0;
                camera_found = 0;
                _log(LOG_GENERAL, "Failed to create camera in worker loop.");
            }
            pthread_mutex_unlock(&camera_mutex);
        }

        // Step 2: Only fetch files if camera is initialized and available
        if (camera_initialized && camera_found > 0)
        {
            download_existing_files_from_camera(image_status);
            _log(LOG_GENERAL, "Existing file download complete.");
        }

        // Update status
        image_status->imported = count_imported_images();
        image_status->uploaded = count_uploaded_images();

        // Step 3: Periodically handle camera events (no reinit)
        static time_t last_camera_check = 0;
        time_t now = time(NULL);
        if (now - last_camera_check >= 2) 
        {
            last_camera_check = now;

            if (camera_initialized && camera_found > 0)
            {
                pthread_mutex_lock(&camera_mutex);

                CameraFile *file;
                gp_file_new(&file);
                CameraEventType event_type;
                void *event_data = NULL;
                int ret = gp_camera_wait_for_event(global_camera, 2000, &event_type, &event_data, global_context);

                if (event_type == GP_EVENT_FILE_ADDED) 
                {
                    CameraFilePath *path = (CameraFilePath *)event_data;
                    _log(LOG_GENERAL, "New file added: %s/%s", path->folder, path->name);
                }

                if (ret != GP_OK) 
                {
                    _log(LOG_GENERAL, "Camera event wait failed: %d", ret);
                    camera_cleanup();
                    camera_initialized = 0;
                    image_status->status = CAMERA_STATUS_NO_CAMERA;
                    camera_found = (ret == GP_ERROR_MODEL_NOT_FOUND) ? 0 : -1;
                }

                gp_file_free(file);
                pthread_mutex_unlock(&camera_mutex);
            }
        }

        // Step 4: Upload images
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
                image_status->status = CAMERA_STATUS_UPLOADING;
                snprintf(path, sizeof(path), "%s/%s", LOCAL_DIR, dir->d_name);

                if (upload_file(path, dir->d_name))
                {
                    mark_uploaded(dir->d_name);
                }
            }
            closedir(d);
        } 
        else if (!internet_up && camera_found > 0) 
        {
            image_status->status = CAMERA_STATUS_IMPORT_ONLY;
        }

        usleep(100000);
    }

    return NULL;
}