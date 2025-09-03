#include <curl/curl.h>
#include <dirent.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

const char *LOCAL_DIR;
const char *TRACK_FILE;
const char *FTP_URL;
const char *FTP_USERPWD;

// compile with: `gcc main.c -o uploader -lcurl -ljson-c`

void load_config() 
{
    const char *config_path = "./config.json";
    FILE *fp = fopen(config_path, "r");
    if (!fp) 
    {
        perror("fopen");
        exit(1);
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *data = malloc(fsize + 1);
    fread(data, 1, fsize, fp);
    data[fsize] = 0;
    fclose(fp);

    struct json_object *parsed_json = json_tokener_parse(data);
    free(data);

    struct json_object *j_local_dir, *j_track_file, *j_ftp_url, *j_ftp_userpwd;
    json_object_object_get_ex(parsed_json, "LOCAL_DIR", &j_local_dir);
    json_object_object_get_ex(parsed_json, "TRACK_FILE", &j_track_file);
    json_object_object_get_ex(parsed_json, "FTP_URL", &j_ftp_url);
    json_object_object_get_ex(parsed_json, "FTP_USERPWD", &j_ftp_userpwd);

    LOCAL_DIR = strdup(json_object_get_string(j_local_dir));
    TRACK_FILE = strdup(json_object_get_string(j_track_file));
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
        return;
    }
    fprintf(f, "%s\n", filename);
    fclose(f);
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
            curl_easy_cleanup(curl);
            return 0;
        }

        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_READDATA, hd_src);

        if (curl_easy_perform(curl) == CURLE_OK)
        {
            success = 1;
        }

        fclose(hd_src);
        curl_easy_cleanup(curl);
    }

    return success;
}

void download_folder(const char *folder) 
{
    pid_t pid = fork();

    if (pid == 0)
    {
        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%%f_%%Y%%m%%d-%%H%%M%%S.jpg", LOCAL_DIR);
        execlp("gphoto2", "gphoto2", "--get-all-files", "--new", "--skip-existing", "--folder", folder, "--filename", filepath, NULL);
        _exit(1);
    } 
    else if (pid > 0) 
    {
        int status;
        waitpid(pid, &status, 0);
    }
}

void download_existing_files() 
{
    FILE *fp = popen("gphoto2 --list-folders", "r");
    if (!fp)
    {
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
            printf("Getting all files from %s...\n", fullpath);
            fflush(stdout);
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

char *get_import_directory() 
{
    static char import_dir[1060];
    char cwd[1024];

    getcwd(cwd, sizeof(cwd));
    snprintf(import_dir, sizeof(import_dir), "%s/import", cwd); // creates a new folder to store imported photos in
    mkdir(import_dir, 0755); // makes the directory, if it doesn't already exist

    return import_dir;
}

int main(int argc, char *argv[]) 
{
    load_config();

    if (argc > 1 && strcmp(argv[1], "--reset") == 0) 
    {
        FILE *f = fopen(TRACK_FILE, "w");
        if (f)
        {
            fclose(f); // truncate file to empty
        }
    }

    pid_t gphoto_pid = -1;

    char *import_dir = get_import_directory();

    while (1) 
    {
        if (gphoto_pid <= 0) 
        {
            download_existing_files();

            gphoto_pid = fork();
            if (gphoto_pid == 0) 
            {
                char filename[1100];
                snprintf(filename, sizeof(filename), "%s/%%f_%%Y%%m%%d-%%H%%M%%S_%%C.jpg", import_dir);
                execlp("gphoto2", "gphoto2", "--wait-event-and-download", "--skip-existing", "--folder", "/", "--filename", filename, NULL);
                _exit(1);
            }
            else if (gphoto_pid < 0) 
            {
                perror("fork failed for wait-event-and-download");
                sleep(2);
                continue;
            }
        }

        int status;
        pid_t ret = waitpid(gphoto_pid, &status, WNOHANG);
        if (ret > 0) 
        {
            fprintf(stderr, "gphoto2 exited, restarting in 2s...\n");
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
                    continue;
                }

                if (is_uploaded(dir->d_name))
                {
                    continue;
                }

                char path[1024];
                snprintf(path, sizeof(path), "%s/%s", LOCAL_DIR, dir->d_name);

                if (upload_file(path, dir->d_name))
                {
                    mark_uploaded(dir->d_name);
                }
            }

            closedir(d);
        }

        sleep(2);
    }

    return 0;
}