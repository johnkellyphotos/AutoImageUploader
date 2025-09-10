#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>

void clear_log_file()
{
    FILE *f = fopen("log.txt", "w");
    if (f)
    {
        fclose(f);
    }
}

void clear_track_file()
{
    FILE *f = fopen(TRACK_FILE, "w");
    if (f)
    {
        fclose(f);
    }
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