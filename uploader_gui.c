#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "log.h"
#include "ui.h"
#include "uploader.h"

int main() 
{
    _log("Program start.");

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

    ImageStatus image_status = {0, 0, 0};
    signal(SIGINT, handle_sigint);
    load_config();

    _log("Initialization complete.");

    pthread_t worker;
    pthread_create(&worker, NULL, import_upload_worker, &image_status);

    pthread_t link_strength;
    pthread_create(&link_strength, NULL, link_poll_thread, NULL);

    pthread_t internet_is_up;
    pthread_create(&internet_is_up, NULL, internet_poll_thread, NULL);

    run_UI(&image_status);

    return 0;
}