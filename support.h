#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <json-c/json.h>
#include <sys/stat.h>

void clear_track_file()
{
    FILE *f = fopen(TRACK_FILE, "w");
    if (f)
    {
        fclose(f);
    }
}

void handle_sigint(int sig)
{
    _log(LOG_GENERAL, "Logging signal interrupt: %i", sig);
    stop_requested = 1;
    exit(1); // kill the progam.
}

void delete_images_in_import_folder()
{
    DIR *dir = opendir(LOCAL_DIR);
    if (!dir)
    {
        return;
    }

    struct dirent *entry;
    char path[1024];

    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_type == DT_REG)
        {
            const char *ext = strrchr(entry->d_name, '.');
            if (ext && (strcasecmp(ext, ".png") == 0 || strcasecmp(ext, ".jpg") == 0 || 
                        strcasecmp(ext, ".jpeg") == 0 || strcasecmp(ext, ".bmp") == 0 || 
                        strcasecmp(ext, ".gif") == 0))
            {
                snprintf(path, sizeof(path), "%s/%s", LOCAL_DIR, entry->d_name);
                unlink(path);
            }
        }
    }

    closedir(dir);
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
        _log(LOG_ERROR, "Configuration file failed to load.");
        perror("fopen");
        exit(1);
    }

    _log(LOG_GENERAL, "Configuration file loaded.");

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