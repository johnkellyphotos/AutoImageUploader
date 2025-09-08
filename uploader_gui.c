#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

volatile int camera_found = 0;
volatile int link_strength_value = 0;
volatile int internet_up = 0;

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

    int full_screen_enabled = 0 ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0;

    SDL_Window *window;
    SDL_Renderer *renderer;
    
    if (full_screen_enabled)
    {
        window = SDL_CreateWindow("Tritium Uploader", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 480, 320, SDL_WINDOW_FULLSCREEN_DESKTOP);
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    }
    else
    {
        window = SDL_CreateWindow("Tritium Uploader", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 480, 320, 0);
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }

    int screen_width, screen_height;
    SDL_GetRendererOutputSize(renderer, &screen_width, &screen_height);

    Button back_button;
    setup_config_buttons(screen_width, screen_height, &back_button);

    Button buttons[2];
    setup_buttons(screen_width, screen_height, buttons, 2);

    int font_size = screen_height / 20;
    TTF_Font *font = TTF_OpenFont("Rubik/Rubik-VariableFont_wght.ttf", font_size);
    if (!font)
    {
        printf("Font load error: %s\n", TTF_GetError()); 
        SDL_DestroyRenderer(renderer); 
        SDL_DestroyWindow(window); 
        TTF_Quit();
        SDL_Quit();
        return 1; 
    }

    ImageStatus image_status = {0, 0, 0};
    signal(SIGINT, handle_sigint);
    load_config();

    _log("Initialization complete.");

    pthread_t worker;
    pthread_create(&worker, NULL, import_upload_worker, &image_status);

    pthread_t cam_thread;
    pthread_create(&cam_thread, NULL, camera_poll_thread, NULL);

    pthread_t link_strength;
    pthread_create(&link_strength, NULL, link_poll_thread, NULL);

    pthread_t internet_is_up;
    pthread_create(&internet_is_up, NULL, internet_poll_thread, NULL);

    SDL_Event e;

    Camera *camera = NULL;
    GPContext *context = gp_context_new();
    int camera_initialized = 0;

    while (!stop_requested) 
    {
        while (SDL_PollEvent(&e)) 
        {
            // prevent killing unresponsive window if not interacted with
            if (e.type == SDL_QUIT)
            {
                stop_requested = 1;
            }
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        render_camera_status(renderer, font, camera_found);
        render_connection_status(renderer, font, link_strength_value);

        switch (current_screen)
        {
            case SCREEN_MAIN:
                render_status_box(renderer, font, &image_status);
                render_buttons(renderer, font, buttons, 2);
                break;

            case SCREEN_CONFIG:
                
                if (!networks_ready)
                {
                    render_text(renderer, font, "Loading networks...", 50, 50);
                }
                else
                {
                    for (int i = 0; i < net_count; i++)
                    {
                        SDL_Color color = {255, 255, 255, 255};
                        SDL_Surface *s = TTF_RenderText_Solid(font, networks[i].ssid, color);
                        SDL_Texture *t = SDL_CreateTextureFromSurface(renderer, s);
                        SDL_Rect r = {50, 50 + i * 30, s->w, s->h};
                        SDL_RenderCopy(renderer, t, NULL, &r);
                        SDL_FreeSurface(s);
                        SDL_DestroyTexture(t);
                    }
                }

                if (selected_network != -1)
                {
                    char password[128] = "";
                    enter_password(renderer, font, networks[selected_network].ssid, password, sizeof(password));
                    connect_to_network(networks[selected_network].ssid, password);
                    selected_network = -1;
                    current_screen = SCREEN_MAIN;
                }

                render_button(renderer, font, &back_button);
                break;
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    if (font)
    {
        TTF_CloseFont(font);
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    return 0;
}