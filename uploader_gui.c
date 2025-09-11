#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>

const char *LOCAL_DIR;
const char *TRACK_FILE = ".track.txt";
const char *FTP_URL;
const char *FTP_USERPWD;

volatile sig_atomic_t stop_requested = 0;

#include "ui_colors.h"
#include "log.h"
#include "support.h"
#include "ftp.h"
#include "ui.h"
#include "uploader.h"

int main(int argc, char *argv[]) 
{
    _log(LOG_GENERAL, "Uploader started... Ready.");

    int full_screen_mode = 0;
    logging_status = LOGGIN_ERROR_ONLY;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--fullscreen") == 0)
        {
            _log(LOG_GENERAL, "Full screen mode enabled.");
            full_screen_mode = 1;
            break;
        }
        else if (strcmp(argv[i], "--log-all") == 0)
        {
            logging_status = LOGGIN_ALL;
        }
        else
        {
            printf("Invalid argument %s. Valid options are '--fullscreen' only.\n", argv[i]);
            _log(LOG_ERROR, "Invalid argument %s. Valid options are '--fullscreen' only.", argv[i]);
            return 10;
        }
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) 
    { 
        printf("SDL Init Error: %s\n", SDL_GetError()); 
        return 1;
    }

    if (TTF_Init() != 0)
    {
        printf("TTF Init Error: %s\n", TTF_GetError());
        SDL_Quit(); 
        return 1;
    }

    Program_status program_status = {0, 0, 0, {0}, {0}};
    signal(SIGINT, handle_sigint);

    load_config();

    _log(LOG_GENERAL, "Initialization complete.");

    // thread to constantly check for attached camera, import images, FTP images
    pthread_t worker;
    pthread_create(&worker, NULL, import_upload_worker, &program_status);

    // thread to constantly check for internet connection. Internet is critical to program use. Program intended to be run in areas with limited internet
    pthread_t internet_is_up;
    pthread_create(&internet_is_up, NULL, internet_poll_thread, NULL);

    // thread to constantly check for connection strength. Internet is critical to program use. Program intended to be run in areas with limited internet
    pthread_t link_strength;
    pthread_create(&link_strength, NULL, link_poll_thread, NULL);

    run_UI(&program_status, full_screen_mode);

    return 0;
}