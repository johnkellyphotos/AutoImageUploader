#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <pthread.h>

#define MAX_NETWORKS 32
#define MAX_PASSWORD 128
#define MAX_CMD 512

volatile int networks_ready = 0;

int selected_network = -1;

int net_count = 0;

typedef struct
{
    char ssid[128];
} WifiNetwork;

WifiNetwork networks[MAX_NETWORKS];

typedef struct
{
    int imported;
    int uploaded;
    int status; // 0 = waiting, 1 = importing, 2 = uploading
} ImageStatus;

typedef struct
{
    int x, y, w, h;
    const char *label;
} Button;

typedef enum
{ 
    SCREEN_MAIN, 
    SCREEN_CONFIG 
} Screen;

volatile sig_atomic_t stop_requested = 0;

Screen current_screen = SCREEN_MAIN;

void render_text(SDL_Renderer *renderer, TTF_Font *font, const char *text, int x, int y) 
{
    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *surface = TTF_RenderText_Solid(font, text, white);
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_Rect dst = {x, y, surface->w, surface->h};
    SDL_RenderCopy(renderer, texture, NULL, &dst);
    SDL_FreeSurface(surface);
    SDL_DestroyTexture(texture);
}

int list_networks(WifiNetwork networks[], int max)
{
    FILE *fp = popen("nmcli -t -f SSID dev wifi", "r");
    if (!fp)
    {
        return 0;
    }

    int count = 0;
    char line[256];

    while (fgets(line, sizeof(line), fp) && count < max)
    {
        line[strcspn(line, "\n")] = 0;
        if (strlen(line) > 0)
        {
            strncpy(networks[count].ssid, line, sizeof(networks[count].ssid));
            networks[count].ssid[sizeof(networks[count].ssid)-1] = 0;
            count++;
        }
    }

    pclose(fp);
    return count;
}

void* scan_networks_thread(void* arg)
{
    net_count = list_networks(networks, MAX_NETWORKS);
    networks_ready = 1;
    return NULL;
}

int select_network(SDL_Renderer *renderer, TTF_Font *font, WifiNetwork networks[], int net_count)
{
    int selected = -1;
    SDL_Event e;
    SDL_Rect net_rects[MAX_NETWORKS];

    while (!stop_requested && selected == -1)
    {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        render_text(renderer, font, "Select Wi-Fi network:", 50, 50);

        int mx, my;
        SDL_GetMouseState(&mx, &my);

        for (int i = 0; i < net_count; i++)
        {
            SDL_Rect r = {50, 50 + i * 40, 220, 30};
            net_rects[i] = r;

            SDL_Color bg = {40, 40, 40, 255};
            if (mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h)
            {
                bg = (SDL_Color){70, 70, 70, 255};
            }

            SDL_SetRenderDrawColor(renderer, bg.r, bg.g, bg.b, 255);
            SDL_RenderFillRect(renderer, &r);

            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderDrawRect(renderer, &r);

            SDL_Surface *s = TTF_RenderText_Solid(font, networks[i].ssid, (SDL_Color){255,255,255,255});
            SDL_Texture *t = SDL_CreateTextureFromSurface(renderer, s);
            SDL_Rect dst = {r.x + 10, r.y + (r.h - s->h)/2, s->w, s->h};
            SDL_RenderCopy(renderer, t, NULL, &dst);
            SDL_FreeSurface(s);
            SDL_DestroyTexture(t);
        }

        SDL_RenderPresent(renderer);

        while (SDL_PollEvent(&e))
        {
            if (e.type == SDL_QUIT)
            {
                return -1;
            }

            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT)
            {
                int mx = e.button.x, my = e.button.y;
                for (int i = 0; i < net_count; i++)
                {
                    if (mx >= net_rects[i].x && mx <= net_rects[i].x + net_rects[i].w &&
                        my >= net_rects[i].y && my <= net_rects[i].y + net_rects[i].h)
                    {
                        selected = i;
                        break;
                    }
                }
            }
        }

        SDL_Delay(16);
    }

    return selected;
}


void enter_password(SDL_Renderer *renderer, TTF_Font *font, const char *ssid, char *password, int max_len)
{
    SDL_StartTextInput();
    int done = 0;
    SDL_Event e;
    password[0] = 0;

    while (!done && !stop_requested)
    {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        char prompt[256];
        snprintf(prompt, sizeof(prompt), "Enter password for %s:", ssid);
        render_text(renderer, font, prompt, 50, 50);
        render_text(renderer, font, password, 50, 100);

        SDL_RenderPresent(renderer);

        while (SDL_PollEvent(&e))
        {
            if (e.type == SDL_QUIT)
            {
                stop_requested = 1;
            }

            if (e.type == SDL_TEXTINPUT)
            {
                strncat(password, e.text.text, max_len - strlen(password) - 1);
            }

            if (e.type == SDL_KEYDOWN)
            {
                if (e.key.keysym.sym == SDLK_RETURN)
                {
                    done = 1;
                }

                if (e.key.keysym.sym == SDLK_BACKSPACE)
                {
                    int len = strlen(password);
                    if (len > 0)
                    {
                        password[len-1] = 0;
                    }
                }
            }
        }
    }

    SDL_StopTextInput();
}

void connect_to_network(const char *ssid, const char *password)
{
    char cmd[MAX_CMD];
    snprintf(cmd, sizeof(cmd), "nmcli device wifi connect '%s' password '%s' &", ssid, password);
    system(cmd);
}

int mouse_over_button(Button *btn, int mx, int my)
{
    return mx >= btn->x && mx <= btn->x + btn->w && my >= btn->y && my <= btn->y + btn->h;
}

void handle_sigint(int sig)
{
    stop_requested = 1;
}

void render_button(SDL_Renderer *renderer, TTF_Font *font, Button *btn)
{
    SDL_Rect rect = {btn->x, btn->y, btn->w, btn->h};
    SDL_SetRenderDrawColor(renderer, 80, 80, 80, 255);
    SDL_RenderFillRect(renderer, &rect);

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDrawRect(renderer, &rect);

    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *surface = TTF_RenderText_Solid(font, btn->label, white);
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);

    int tx = btn->x + (btn->w - surface->w) / 2;
    int ty = btn->y + (btn->h - surface->h) / 2;
    SDL_Rect dst = {tx, ty, surface->w, surface->h};
    SDL_RenderCopy(renderer, texture, NULL, &dst);

    SDL_FreeSurface(surface);
    SDL_DestroyTexture(texture);
}

void setup_config_buttons(int screen_w, int screen_h, Button *back_button)
{
    int margin = screen_w / 50;
    int spacing = screen_w / 50;
    int btn_w = (screen_w - margin*2 - spacing) / 2;
    int btn_h = screen_h / 15;
    int y = screen_h - btn_h - margin;

    back_button->x = margin;
    back_button->y = y;
    back_button->w = btn_w;
    back_button->h = btn_h;
    back_button->label = "Back";
}

void render_buttons(SDL_Renderer *renderer, TTF_Font *font, Button buttons[], int count)
{
    for (int i = 0; i < count; i++)
    {
        render_button(renderer, font, &buttons[i]);
    }
}

void setup_buttons(int screen_w, int screen_h, Button buttons[], int count)
{
    int margin = screen_w / 50;
    int spacing = screen_w / 50;
    int btn_w = (screen_w - margin*2 - spacing) / 2;
    int btn_h = screen_h / 15;
    int y = screen_h - btn_h - margin;

    buttons[0].x = margin;
    buttons[0].y = y;
    buttons[0].w = btn_w;
    buttons[0].h = btn_h;
    buttons[0].label = "Select a network";

    buttons[1].x = margin + btn_w + spacing;
    buttons[1].y = y;
    buttons[1].w = btn_w;
    buttons[1].h = btn_h;
    buttons[1].label = "Clear imports";
}

void render_signal_indicator(SDL_Renderer *renderer, int x, int y, int total_height, int total_width, int strength_percent)
{
    int bars = 4;
    int bar_spacing = total_width / 10;
    int bar_width = (total_width - (bars - 1) * bar_spacing) / bars;

    SDL_SetRenderDrawColor(renderer, 50, 50, 50, 255);
    for (int i = 0; i < bars; i++)
    {
        int bar_height = ((i + 1) * total_height) / bars;
        SDL_Rect bg = { x + i * (bar_width + bar_spacing), y + total_height - bar_height, bar_width, bar_height };
        SDL_RenderFillRect(renderer, &bg);
    }

    int active_bars = (strength_percent * bars) / 100;
    if (active_bars > bars)
    {
        active_bars = bars;
    }

    SDL_SetRenderDrawColor(renderer, 0, 200, 0, 255);

    for (int i = 0; i < active_bars; i++)
    {
        int bar_height = ((i + 1) * total_height) / bars;
        SDL_Rect fg = { x + i * (bar_width + bar_spacing), y + total_height - bar_height, bar_width, bar_height };
        SDL_RenderFillRect(renderer, &fg);
    }
}

int get_link_strength()
{
    FILE *f = fopen("/proc/net/wireless", "r");
    if (!f)
    {
        return 0;
    }

    char line[256];
    fgets(line, sizeof(line), f);
    fgets(line, sizeof(line), f);
    float link = 0;
    sscanf(line + 29, "%f", &link);
    fclose(f);
    int strength = (int)(link * 100 / 70);

    if (strength > 100)
    {
        strength = 100;
    }

    if (strength < 0)
    {
        strength = 0;
    }

    return strength;
}

int internet_connected()
{
    return system("ping -c 1 8.8.8.8 > /dev/null 2>&1") == 0;
}

void render_connection_status(SDL_Renderer *renderer, TTF_Font *font, int link_strength)
{
    SDL_Color white = {255, 255, 255, 255};
    const char *status_text = internet_connected() ? "Connected" : "Not connected";

    int w, h;
    SDL_GetRendererOutputSize(renderer, &w, &h);

    SDL_Surface *surface = TTF_RenderText_Solid(font, status_text, white);
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);

    int bar_size = (int)(surface->h * 0.8);
    int spacing = 10;
    int total_width = surface->w + spacing + bar_size;
    int start_x = w - total_width - 10;
    int y_pos = 11;

    SDL_Rect dst = {start_x, y_pos, surface->w, surface->h};
    SDL_RenderCopy(renderer, texture, NULL, &dst);

    render_signal_indicator(renderer, start_x + surface->w + spacing, y_pos + (surface->h - bar_size) / 2, bar_size, bar_size, link_strength);

    SDL_FreeSurface(surface);
    SDL_DestroyTexture(texture);
}

void render_status_box(SDL_Renderer *renderer, TTF_Font *font, ImageStatus *status)
{
    int w, h;
    SDL_GetRendererOutputSize(renderer, &w, &h);

    int margin = w / 50;
    int spacing = w / 50;

    char imported_text[64];
    snprintf(imported_text, sizeof(imported_text), "%i image%s imported", status->imported, status->imported == 1 ? "" : "s");
    char uploaded_text[64];
    snprintf(uploaded_text, sizeof(uploaded_text), "%i image%s sent to server", status->uploaded, status->uploaded == 1 ? "" : "s");

    int y_offset = 50;
    render_text(renderer, font, imported_text, margin, y_offset);
    y_offset += h / 30 + 5;

    render_text(renderer, font, uploaded_text, margin, y_offset);
    y_offset += h / 30 + 10;

    const char *status_str = (status->status == 0) ? "Waiting for images" :
                             (status->status == 1) ? "Importing images" : "Uploading images";

    render_text(renderer, font, status_str, margin, y_offset);
}

int main() 
{
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

    int font_size = screen_height / 30;
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

    ImageStatus status = {0, 0, 0};
    SDL_Event e;

    signal(SIGINT, handle_sigint);

    while (!stop_requested) 
    {
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

        switch (current_screen)
        {
            case SCREEN_MAIN:
                int strength = get_link_strength();
                render_connection_status(renderer, font, strength);
                render_status_box(renderer, font, &status);
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

        // Example progression - remove after testing
        status.imported += 1;
        status.uploaded += (status.status == 2) ? 1 : 0;
        status.status = (status.status + 1) % 3;
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
