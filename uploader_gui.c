#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

volatile int camera_found = 0;
volatile int link_strength_value = 0;
volatile int internet_up = 0;
volatile int networks_ready = 0;
volatile int select_network_index = -1;
volatile int has_attempted_connection = 0;
volatile int ready_for_password = 0;

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

    int full_screen_enabled = 1 ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0;

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

    Button retry_button;
    setup_retry_button(screen_width, screen_height, &retry_button);

    Button buttons[2];
    setup_buttons(screen_width, screen_height, buttons);

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

    pthread_t link_strength;
    pthread_create(&link_strength, NULL, link_poll_thread, NULL);

    pthread_t internet_is_up;
    pthread_create(&internet_is_up, NULL, internet_poll_thread, NULL);

    SDL_Event e;

    while (!stop_requested) 
    {
        while (SDL_PollEvent(&e)) 
        {
            // prevent killing unresponsive window if not interacted with
            if (e.type == SDL_QUIT)
            {
                stop_requested = 1;
            }

            // determine if a button was clicked
            if (e.type == SDL_MOUSEBUTTONDOWN)
            {
                int mx = e.button.x;
                int my = e.button.y;

                for (int i = 0; i < 2; i++)
                {
                    if (button_is_pressed(buttons[i], mx, my))
                    {
                        if (i == 0)
                        {
                            if (current_screen == SCREEN_MAIN)
                            {
                                current_screen = SCREEN_NETWORK_CONFIG;
                            }
                            else
                            {
                                current_screen = SCREEN_MAIN;
                            }
                        }
                        else
                        {
                            // config button not set up yet.
                        }
                    }
                }
            }
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        render_header(renderer, font);

        switch (current_screen)
        {
            case SCREEN_MAIN:
                render_status_box(renderer, font, &image_status);
                render_buttons(renderer, font, buttons, 2);
                break;

            case SCREEN_NETWORK_CONFIG:

                if (!ready_for_password)
                {
                    if (!networks_ready)
                    {
                        // loading network list
                        render_loading_network_text(renderer, font);
                        pthread_t scan_networks;
                        pthread_create(&scan_networks, NULL, scan_networks_thread, NULL);
                        pthread_detach(scan_networks);
                    }
                    else if (select_network_index < 0)
                    {
                        // needs to select a network
                        select_network_index = render_select_network(renderer, font, networks, net_count, back_button, retry_button);
                        printf("Selected network is index: %i => %s\n\n", select_network_index, networks[select_network_index].ssid);

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
                    else if (select_network_index >= 0)
                    {
                        printf("Selected network is index: %i => %s\n\n", select_network_index, networks[select_network_index].ssid);
                        // network has been selected. Attempt to connect without password
                        if (!has_attempted_connection && connect_to_network(networks[select_network_index].ssid, NULL) != 0)
                        {
                            has_attempted_connection = 1;
                            ready_for_password = 1;
                            
                            // render_password_prompt(renderer, font, networks[select_network_index].ssid, networks[select_network_index].password, MAX_PASSWORD);
                            
                            if (strlen(networks[select_network_index].password) > 0)
                            {
                                connect_to_network(networks[select_network_index].ssid, networks[select_network_index].password);
                            }
                        }
                    }
                }

                if (select_network_index >= 0 && ready_for_password)
                {
                    printf("Wanting password now...\n");
                    char password[128] = "";
                    // enter_password(renderer, font, networks[selected_network].ssid, password, sizeof(password));
                    // connect_to_network(networks[selected_network].ssid, password);
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