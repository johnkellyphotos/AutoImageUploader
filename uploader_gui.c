#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

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

    SDL_Window *window = SDL_CreateWindow("Uploader GUI", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 320, 480, SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

    int screen_width, screen_height;
    SDL_GetRendererOutputSize(renderer, &screen_width, &screen_height);

    Button back_button;
    setup_config_buttons(screen_width, screen_height, &back_button);

    Button buttons[2]; // main screen buttons
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
    SDL_Event e;

    int camera_detected = 0;

    signal(SIGINT, handle_sigint);

    load_config();

    pid_t gphoto_pid = -1; // process ID for gphoto2

    _log("Initialization complete.");

    while (!stop_requested) 
    {
        if (gphoto_pid <= 0) 
        {
            if (!camera_present()) 
            {
                camera_detected = 0;
                _log("No camera detected. Waiting 2s before retry.");
            }
            else
            {
                camera_detected = 1;
                download_existing_files();

                image_status.imported = 1;
                image_status.status = 1;

                gphoto_pid = fork();
                if (gphoto_pid == 0) 
                {
                    char filename[1100];
                    snprintf(filename, sizeof(filename), "%s/%%f_%%Y%%m%%d-%%H%%M%%S_%%C.jpg", LOCAL_DIR);

                    _log("Saving file from camera to to %s.", filename);
                    execlp("gphoto2", "gphoto2", "--wait-event-and-download", "--skip-existing", "--folder", "/", "--filename", filename, NULL); // starts child process to download image

                    _log("Failed to save file from camera to to %s.", filename);
                    _exit(1);
                }
                else if (gphoto_pid < 0) 
                {
                    _log("Failed to fork process. continuing after 2 second wait...");
                    perror("fork failed for wait-event-and-download");
                    continue;
                }
            }
        }

        int status;
        pid_t ret = waitpid(gphoto_pid, &status, WNOHANG);
        if (ret == -1) {
            perror("waitpid failed");
            gphoto_pid = -1;
        }
        else if (ret > 0) 
        {
            if (WIFEXITED(status)) 
            {
                int code = WEXITSTATUS(status);
                if (code == 0) 
                {
                    _log("Task completed and exited cleanly (process done).");
                }
                 else if (code == 1) 
                 {
                    _log("Task completed (likely no more events / camera removed).");
                }
                else
                {
                    _log("gphoto2 exited with error code %d (likely camera disconnect).", code);
                }
            } 
            else if (WIFSIGNALED(status)) 
            {
                int sig = WTERMSIG(status);
                _log("gphoto2 terminated by signal %d (likely camera disconnect).", sig);
            }

            gphoto_pid = -1;
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
                    _log("Skipping upload of non JPEG image: %s because it is extension type: %s.", dir->d_name, ext ? ext : "(none)");
                    continue;
                }

                if (is_uploaded(dir->d_name))
                {
                    _log("Skipping upload of %s because it marked as uploaded in track file.", dir->d_name);
                    continue;
                }

                char path[1024];
                snprintf(path, sizeof(path), "%s/%s", LOCAL_DIR, dir->d_name);

                _log("Attempting to upload file: %s.", dir->d_name);
                image_status.status = 2;
                if (upload_file(path, dir->d_name))
                {
                    image_status.uploaded += 1;
                    image_status.status = 0;
                    _log("Upload complete.");
                    mark_uploaded(dir->d_name);
                }
                else
                {
                    _log("Upload failed for file: %s.", dir->d_name);
                }
            }

            closedir(d);
        }
        else
        {
            _log("Failed to open directory %s.", LOCAL_DIR);
        }

        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            if (e.type == SDL_QUIT)
            {
                stop_requested = 1;
            }

            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT)
            {
                int mx = e.button.x, my = e.button.y;

                switch (current_screen)
                {
                    case SCREEN_MAIN:
                        if (mouse_over_button(&buttons[0], mx, my))
                        {
                            networks_ready = 0;
                            pthread_t thread;
                            pthread_create(&thread, NULL, scan_networks_thread, NULL);
                            current_screen = SCREEN_CONFIG;
                        }
                    break;

                    case SCREEN_CONFIG:
                        if (mouse_over_button(&back_button, mx, my))
                        {
                            current_screen = SCREEN_MAIN;
                        }
                        else
                        {
                            for (int i = 0; i < net_count; i++)
                            {
                                SDL_Rect r = {50, 50 + i * 30, 200, 25};
                                if (mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h)
                                {
                                    selected_network = i;
                                }
                            }
                        }
                    break;
                }
            }
        }
        
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        int strength = get_link_strength();
        render_camera_status(renderer, font, camera_detected);
        render_connection_status(renderer, font, strength);

        switch (current_screen)
        {
            case SCREEN_MAIN:
                render_status_box(renderer, font, &image_status);
                render_buttons(renderer, font, buttons, 2);
            break;

            case SCREEN_CONFIG:
                render_button(renderer, font, &back_button);

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
        SDL_Delay(500); // app refresh rate, 500ms
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
