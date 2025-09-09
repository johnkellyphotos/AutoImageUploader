#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <libusb-1.0/libusb.h>
#include <unistd.h>
#include "buttons.h"

#define MAX_NETWORKS 5
#define MAX_PASSWORD 128
#define MAX_CMD 512

#define UI_PADDING_TOP 30
#define UI_PADDING_LEFT 10

// use vilatilie ints because these will be accessed directly without optimization. (variable updated across threads)
volatile int selected_network = -1;
volatile int net_count = 0;
volatile int link_strength_value = 0;
volatile int internet_up = 0;
volatile int networks_ready = 0;
volatile int networks_scanned = 0;
volatile int select_network_index = -1;
volatile int has_attempted_connection = 0;
volatile int network_connect_complete_status = 0;
volatile int network_connect_complete = 0;
volatile int camera_found = 0;

pid_t conn_pid = 0;
int conn_status = -1;

typedef struct {
    volatile int x;
    volatile int y;
} LastClick;

LastClick last_click = {0, 0};

typedef struct
{
    char ssid[128];
} WifiNetwork;

WifiNetwork networks[MAX_NETWORKS];

typedef struct
{
    int imported;
    int uploaded;
    int status; // 0 = waiting, 1 = importing, 2 = uploading, 3 = No internet, import only
} ImageStatus;

volatile sig_atomic_t stop_requested = 0;

Screen current_screen = SCREEN_MAIN;

int number_dots_for_loading_screen = 1;
int screen_refresh_count = 0;

int navigation_button_is_pressed(Button button, int mx, int my)
{
    // checks if the click event occured inside the boundaries of the button. Does not know Z-index.
    return mx >= button.x && mx <= button.x + button.w && my >= button.y && my <= button.y + button.h;
}

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

void create_text_with_dynamic_elipsis(char *str, int max_string_length)
{
    char dots[5] = {0};
    for (int x = 0; x < number_dots_for_loading_screen && x < 4; x++)
    {
        dots[x] = '.';
    }

    if (screen_refresh_count++ > 30)
    {
        // roughly once per half second based on 60 refreshes / sec
        number_dots_for_loading_screen = (number_dots_for_loading_screen >= 4) ? 1 : number_dots_for_loading_screen + 1;
        screen_refresh_count = 0;
    }

    char original[max_string_length];
    strncpy(original, str, max_string_length - 1);
    original[max_string_length - 1] = '\0'; // safety max null terminator
    snprintf(str, max_string_length, "%s%s", original, dots);
}

void render_loading_network_text(SDL_Renderer *renderer, TTF_Font *font)
{
    char loading_statement[64] = "Loading networks";
    create_text_with_dynamic_elipsis(loading_statement, 64);
    render_text(renderer, font, loading_statement, UI_PADDING_LEFT, UI_PADDING_TOP);
}

int list_networks(int max)
{
    // reset array to empty
    memset(networks, 0, sizeof(networks));

    FILE *fp = popen("nmcli -t -f SSID dev wifi", "r");
    if (!fp)
    {
        _log("Can not open file tto read Wifi status.");
        return 0;
    }

    _log("Attempting to read Wifi information...");

    int count = 0;
    char line[256];

    while (fgets(line, sizeof(line), fp) && count < max)
    {
        line[strcspn(line, "\n")] = 0;
        if (strlen(line) > 0)
        {
            int number_networks = count;

            int add_to_network_list = 1;
            for (int x=0; x<number_networks; x++)
            {
                if (strcmp(networks[x].ssid, line) == 0)
                {
                    // already in network list (avoid duplicates)
                    add_to_network_list = 0;
                    break;
                }
            }

            if (add_to_network_list)
            {
                _log("Adding network %s to network list.", line);
                strncpy(networks[count].ssid, line, sizeof(networks[count].ssid));
                networks[count].ssid[sizeof(networks[count].ssid)-1] = 0;
                count++;
            }
            else
            {
                _log("Declining to add network %s to network list because it is a duplicate.", line);
            }
        }
    }

    pclose(fp);
    return count;
}

void* scan_networks_thread()
{
    net_count = list_networks(MAX_NETWORKS);
    networks_ready = 1;
    return NULL;
}

void clip_string(char *dest, const char *src, int n)
{
    int i;
    for (i = 0; i < n && src[i]; i++)
    {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

int connect_to_network(const char *ssid)
{
    pid_t pid = fork();
    if (pid < 0)
    {
        return -1;
    }
    if (pid == 0)
    {
        execlp("nmcli", "nmcli", "device", "wifi", "connect", ssid, NULL);
        _exit(127);
    } 
    else
    {
        int status;
        waitpid(pid, &status, 0);

        if (WIFEXITED(status))
        {
            return WEXITSTATUS(status);
        }
        else
        {
            return -1;
        }
    }
}

void render_button(SDL_Renderer *renderer, TTF_Font *font, Button btn)
{
    SDL_Rect rect = {btn.x, btn.y, btn.w, btn.h};
    SDL_SetRenderDrawColor(renderer, 80, 80, 80, 255);
    SDL_RenderFillRect(renderer, &rect);

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDrawRect(renderer, &rect);

    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *surface = TTF_RenderText_Solid(font, btn.label, white);
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);

    int tx = btn.x + (btn.w - surface->w) / 2;
    int ty = btn.y + (btn.h - surface->h) / 2;
    SDL_Rect dst = {tx, ty, surface->w, surface->h};
    SDL_RenderCopy(renderer, texture, NULL, &dst);

    SDL_FreeSurface(surface);
    SDL_DestroyTexture(texture);
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
    if (!internet_up)
    {
        return 0;
    }

    FILE *f = fopen("/proc/net/wireless", "r");
    if (!f)
    {
        return 0;
    }

    char line[256];
    fgets(line, sizeof(line), f);
    fgets(line, sizeof(line), f);
    fgets(line, sizeof(line), f);

    char iface[16];
    float status, link, level, noise;
    if (sscanf(line, " %15[^:]: %f %f %f %f %f %f", iface, &status, &link, &level, &noise, &noise, &noise) < 4)
    {
        fclose(f);
        return 0;
    }

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

void* link_poll_thread()
{
    while (!stop_requested)
    {
        link_strength_value = get_link_strength();
        usleep(1000000);
    }
    return NULL;
}

void* internet_poll_thread()
{
    while (!stop_requested)
    {
        internet_up = (system("ping -c 1 8.8.8.8 > /dev/null 2>&1") == 0);
        sleep(2);
    }
    return NULL;
}

void render_camera_status(SDL_Renderer *renderer, TTF_Font *font)
{
    SDL_Color font_color;
    switch (camera_found)
    {
        case -1:
            font_color = (SDL_Color){255, 255, 0, 255};
            break;
        case 1:
            font_color = (SDL_Color){55, 255, 55, 255};
            break;
        default:
            font_color = (SDL_Color){255, 0, 0, 255};
            break;
    }

    const char *status_text;
    switch (camera_found)
    {
        case -1:
            status_text = "Camera detected - no communication";
            break;
        case 1:
            status_text = "Camera detected";
            break;
        default:
            status_text = "No camera detected";
            break;
    }

    int screen_width, screen_height;
    SDL_GetRendererOutputSize(renderer, &screen_width, &screen_height);

    SDL_Surface *surface = TTF_RenderText_Solid(font, status_text, font_color);
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);

    int start_x = UI_PADDING_LEFT;
    int y_pos = UI_PADDING_LEFT;

    SDL_Rect dst = {start_x, y_pos, surface->w, surface->h};
    SDL_RenderCopy(renderer, texture, NULL, &dst);

    SDL_FreeSurface(surface);
    SDL_DestroyTexture(texture);
}

void render_connection_status(SDL_Renderer *renderer, TTF_Font *font)
{
    SDL_Color font_color = internet_up
        ? (SDL_Color){55, 255, 55, 255} 
        : (SDL_Color){255, 0, 0, 255};
    const char *status_text = internet_up ? "Internet connected" : "No internet";

    int screen_width, screen_height;
    SDL_GetRendererOutputSize(renderer, &screen_width, &screen_height);

    SDL_Surface *surface = TTF_RenderText_Solid(font, status_text, font_color);
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);

    int bar_size = (int)(surface->h * 0.66);
    int spacing = 10;
    int total_width = surface->w + spacing + bar_size;
    int start_x = screen_width - total_width - 10;
    int y_pos = 10;

    SDL_Rect dst = {start_x, y_pos, surface->w, surface->h};
    SDL_RenderCopy(renderer, texture, NULL, &dst);

    render_signal_indicator(renderer, start_x + surface->w + spacing, y_pos + (surface->h - bar_size) / 2, bar_size, bar_size, link_strength_value);

    SDL_FreeSurface(surface);
    SDL_DestroyTexture(texture);
}

void render_status_box(SDL_Renderer *renderer, TTF_Font *font, ImageStatus *image_status)
{
    int w, h;
    SDL_GetRendererOutputSize(renderer, &w, &h);

    int margin = UI_PADDING_LEFT;

    char imported_text[64];
    snprintf(imported_text, sizeof(imported_text), "%i image%s imported", image_status->imported, image_status->imported == 1 ? "" : "s");
    char uploaded_text[64];
    snprintf(uploaded_text, sizeof(uploaded_text), "%i image%s sent to server", image_status->uploaded, image_status->uploaded == 1 ? "" : "s");

    int y_offset = UI_PADDING_TOP;
    render_text(renderer, font, imported_text, margin, y_offset);
    y_offset += h / 30 + 5;

    render_text(renderer, font, uploaded_text, margin, y_offset);
    y_offset += h / 30 + 10;

    const char *status_str;
    switch (image_status->status)
    {
        case 0:
            status_str = "Waiting for images";
            break;
        case 1:
            status_str = "Importing images";
            break;
        case 2:
            status_str = "Uploading images";
            break;
        case 3:
            status_str = "Importing images only - no internet";
            break;
    }

    render_text(renderer, font, status_str, margin, y_offset);
}

void render_header(SDL_Renderer *renderer, TTF_Font *font)
{
    render_camera_status(renderer, font); 
    render_connection_status(renderer, font);   
}

void render_main_screen(SDL_Renderer * renderer, TTF_Font * font, ImageStatus *image_status, Navigation_buttons navigation_buttons)
{
    render_status_box(renderer, font, image_status);
    render_button(renderer, font, navigation_buttons.select_network);
    render_button(renderer, font, navigation_buttons.clear_import);
}

void render_network_connection_complete_screen(SDL_Renderer * renderer, TTF_Font * font, Navigation_buttons navigation_buttons)
{
    char loading_statement[64];
    if (network_connect_complete_status < 0)
    {
        strcpy(loading_statement, "Failed to connect to network.");
        render_text(renderer, font, loading_statement, UI_PADDING_LEFT, UI_PADDING_TOP);
        render_button(renderer, font, navigation_buttons.back);
        render_button(renderer, font, navigation_buttons.retry);
    }
    else
    {
        strcpy(loading_statement, "Network connection successful.");
        render_text(renderer, font, loading_statement, UI_PADDING_LEFT, UI_PADDING_TOP);
        render_button(renderer, font, navigation_buttons.back);
    }
}

void render_loading_network_list_screen(SDL_Renderer * renderer, TTF_Font * font, Navigation_buttons navigation_buttons)
{
    render_loading_network_text(renderer, font);
    if (!networks_scanned)
    {
        networks_scanned = 1;
        pthread_t scan_networks;
        pthread_create(&scan_networks, NULL, scan_networks_thread, NULL);
        pthread_detach(scan_networks);
    }

    render_button(renderer, font, navigation_buttons.back);
}

void render_no_network_found(SDL_Renderer * renderer, TTF_Font * font, Navigation_buttons navigation_buttons)
{
    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *title_surf = TTF_RenderText_Solid(font, "No networks found. Retry?", white);
    SDL_Texture *title_tex = SDL_CreateTextureFromSurface(renderer, title_surf);
    SDL_Rect title_rect = {UI_PADDING_LEFT, UI_PADDING_TOP, title_surf->w, title_surf->h};

    SDL_RenderCopy(renderer, title_tex, NULL, &title_rect);
    SDL_FreeSurface(title_surf);
    SDL_DestroyTexture(title_tex);

    render_button(renderer, font, navigation_buttons.back);
    render_button(renderer, font, navigation_buttons.retry);

    SDL_RenderPresent(renderer);
}

void render_select_network_screen(SDL_Renderer * renderer, TTF_Font * font, Navigation_buttons navigation_buttons)
{
    if (net_count < 1)
    {
        select_network_index = -1;
        render_no_network_found(renderer, font, navigation_buttons);
        return;
    }

    int select_text_height = 30;

    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *title_surf = TTF_RenderText_Solid(font, "Select Wi-Fi network:", white);
    SDL_Texture *title_tex = SDL_CreateTextureFromSurface(renderer, title_surf);
    SDL_Rect title_rect = {UI_PADDING_LEFT, UI_PADDING_TOP, title_surf->w, title_surf->h};
    SDL_RenderCopy(renderer, title_tex, NULL, &title_rect);
    SDL_FreeSurface(title_surf);
    SDL_DestroyTexture(title_tex);

    for (int i = 0; i < net_count; i++)
    {
        SDL_Rect r = {UI_PADDING_LEFT, UI_PADDING_TOP + select_text_height + i * 30, 320, 24};

        SDL_SetRenderDrawColor(renderer, 40, 40, 40, 255);
        SDL_RenderFillRect(renderer, &r);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderDrawRect(renderer, &r);

        char network_name[32];
        clip_string(network_name, networks[i].ssid, 32);
        SDL_Surface *s = TTF_RenderText_Solid(font, network_name, white);
        SDL_Texture *t = SDL_CreateTextureFromSurface(renderer, s);
        SDL_Rect dst = {r.x + 10, r.y + (r.h - s->h)/2, s->w, s->h};
        SDL_RenderCopy(renderer, t, NULL, &dst);
        SDL_FreeSurface(s);
        SDL_DestroyTexture(t);

        if (last_click.x >= r.x && last_click.x <= r.x + r.w && last_click.y >= r.y && last_click.y <= r.y + r.h)
        {
            select_network_index = i;
        }
    }

    render_button(renderer, font, navigation_buttons.back);
    SDL_RenderPresent(renderer);
}

void render_attempting_network_connection_screen(SDL_Renderer *renderer, TTF_Font *font, Navigation_buttons navigation_buttons)
{
    networks_scanned = 0; // reset networks have been scanned. If they hit back, or the scan completes, we want a new network list

    char loading_statement[64] = "Attempting to connect to network";
    create_text_with_dynamic_elipsis(loading_statement, 64);

    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *title_surf = TTF_RenderText_Solid(font, loading_statement, white);
    SDL_Texture *title_tex = SDL_CreateTextureFromSurface(renderer, title_surf);
    SDL_Rect title_rect = {UI_PADDING_LEFT, UI_PADDING_TOP, title_surf->w, title_surf->h};

    SDL_RenderCopy(renderer, title_tex, NULL, &title_rect);
    SDL_FreeSurface(title_surf);
    SDL_DestroyTexture(title_tex);
    SDL_RenderPresent(renderer);

    if (!has_attempted_connection)
    {
        _log("Attempting to connect to network...");
        network_connect_complete_status = 0;
        network_connect_complete = 0;
        has_attempted_connection = 1;
        conn_pid = fork();
        if (conn_pid == 0)
        {
            // call the existing blocking function in the child
            _log("Connection attempt initiated.");
            int ret = connect_to_network(networks[select_network_index].ssid);
            _exit(ret);
        }
    }

    if (conn_pid > 0)
    {
        int status;
        pid_t result = waitpid(conn_pid, &status, WNOHANG);
        if (result > 0)
        {
            if (WIFEXITED(status))
            {
                conn_status = WEXITSTATUS(status);
            }

            conn_pid = 0;

            if (conn_status != 0)
            {
                _log("Network connection failed: %d\n", conn_status);
                network_connect_complete_status = -1;
            }
            else
            {
                _log("Network connected successfully\n");
                network_connect_complete_status = 1;
            }

            network_connect_complete = 1;
            networks_ready = 0;
            select_network_index = -1;
            has_attempted_connection = 0;
        }
    }

    render_button(renderer, font, navigation_buttons.back);
}

void render_network_config_screen(SDL_Renderer * renderer, TTF_Font * font, Navigation_buttons navigation_buttons)
{
    if (network_connect_complete == 1 && network_connect_complete_status != 0)
    {
        render_network_connection_complete_screen(renderer, font, navigation_buttons);
    }
    else if (!networks_ready)
    {
        render_loading_network_list_screen(renderer, font, navigation_buttons);
    }
    else if (select_network_index < 0)
    {
        render_select_network_screen(renderer, font, navigation_buttons);
    }
    else if (select_network_index >= 0 && network_connect_complete == 0)
    {
        render_attempting_network_connection_screen(renderer, font, navigation_buttons);
    }
}

void render_frame(SDL_Renderer * renderer, TTF_Font * font, ImageStatus *image_status, Navigation_buttons navigation_buttons)
{
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    render_header(renderer, font);

    switch (current_screen)
    {
        case SCREEN_MAIN:
            render_main_screen(renderer, font, image_status, navigation_buttons);
            break;

        case SCREEN_NETWORK_CONFIG:
            render_network_config_screen(renderer, font, navigation_buttons);
            break;
    }

    SDL_RenderPresent(renderer);
    SDL_Delay(16);
}

void handle_events(SDL_Event e, Navigation_buttons navigation_buttons)
{
    while (SDL_PollEvent(&e)) 
    {
        // Detect if program has been closed by user
        if (e.type == SDL_QUIT)
        {
            stop_requested = 1;
        }

        // determine if a button was clicked
        if (e.type == SDL_MOUSEBUTTONDOWN)
        {
            last_click.x = e.button.x;
            last_click.y = e.button.y;

            switch (current_screen)
            {
                case SCREEN_MAIN:
                    if (navigation_button_is_pressed(navigation_buttons.select_network, last_click.x, last_click.y))
                    {
                        current_screen = navigation_buttons.select_network.target_screen;
                    }
                    else if (navigation_button_is_pressed(navigation_buttons.clear_import, last_click.x, last_click.y))
                    {
                        current_screen = navigation_buttons.clear_import.target_screen;
                    }
                    break;

                case SCREEN_NETWORK_CONFIG:
                    if (navigation_button_is_pressed(navigation_buttons.back, last_click.x, last_click.y))
                    {
                        networks_ready = 0;
                        networks_scanned = 0; // reset network scan status
                        select_network_index = -1;
                        network_connect_complete = 0;
                        network_connect_complete_status = 0;
                        current_screen = navigation_buttons.back.target_screen;
                    }
                    else if (navigation_button_is_pressed(navigation_buttons.retry, last_click.x, last_click.y))
                    {
                        current_screen = navigation_buttons.retry.target_screen;
                    }
                    break;
            }
        }
    }
}

void run_UI(ImageStatus *image_status)
{
    int full_screen_enabled = 0 ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0; // 0 for testing, 1 for production on Pi

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

    int font_size = screen_height / 20;
    TTF_Font *font = TTF_OpenFont("Rubik/Rubik-VariableFont_wght.ttf", font_size);
    if (!font)
    {
        printf("Font load error: %s\n", TTF_GetError()); 
        SDL_DestroyRenderer(renderer); 
        SDL_DestroyWindow(window); 
        TTF_Quit();
        SDL_Quit();
        return; 
    }

    Navigation_buttons navigation_buttons = initialize_navigation_buttons(screen_width, screen_height);
    SDL_Event e;

    while (!stop_requested) 
    {
        // run the program continuously unless a program stop has been requested
        handle_events(e, navigation_buttons);
        render_frame(renderer, font, image_status, navigation_buttons);
    }

    TTF_CloseFont(font);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
}