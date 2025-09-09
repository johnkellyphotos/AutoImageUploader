#include <signal.h>
#include <libusb-1.0/libusb.h>
#include <unistd.h>

#define MAX_NETWORKS 3
#define MAX_PASSWORD 128
#define MAX_CMD 512

int selected_network = -1;
int net_count = 0;

typedef struct
{
    char ssid[128];
    char password[128];
} WifiNetwork;

WifiNetwork networks[MAX_NETWORKS];

typedef struct
{
    int imported;
    int uploaded;
    int status; // 0 = waiting, 1 = importing, 2 = uploading, 3 = No internet, import only
} ImageStatus;

typedef struct
{
    int x, y, w, h;
    const char *label;
} Button;

typedef enum
{ 
    SCREEN_MAIN, 
    SCREEN_NETWORK_CONFIG
} Screen;

volatile sig_atomic_t stop_requested = 0;

Screen current_screen = SCREEN_MAIN;

int number_dots_for_loading_screen = 1;
int screen_refresh_count = 0;

int button_is_pressed(Button button, int mx, int my)
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

void render_loading_network_text(SDL_Renderer *renderer, TTF_Font *font)
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

    char loading_statement[64];
    snprintf(loading_statement, sizeof(loading_statement), "Loading networks%s", dots);

    render_text(renderer, font, loading_statement, 10, 50);
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

int connect_to_network(const char *ssid, const char *password)
{
    char cmd[512];
    if (password && strlen(password) > 0)
    {
        snprintf(cmd, sizeof(cmd), "nmcli device wifi connect '%s' password '%s'", ssid, password);
    }
    else
    {
        snprintf(cmd, sizeof(cmd), "nmcli device wifi connect '%s'", ssid);
    }

    int ret = system(cmd);
    return WEXITSTATUS(ret);
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

int render_select_network(SDL_Renderer *renderer, TTF_Font *font, WifiNetwork networks[], int net_count, Button back_button, Button retry_button)
{
    int selected = -1;
    SDL_Event e;
    SDL_Rect net_rects[MAX_NETWORKS];

    int screen_width, screen_height;
    SDL_GetRendererOutputSize(renderer, &screen_width, &screen_height);

    while (!stop_requested && selected == -1)
    {
        SDL_Rect clear_area = {0, 40, screen_width, screen_height - 40 - 60};
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderFillRect(renderer, &clear_area);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

        int top_margin = 30, left_margin = 20, select_text_height = 30;
        SDL_Color white = {255, 255, 255, 255};

        if (net_count < 1)
        {
            SDL_Surface *title_surf = TTF_RenderText_Solid(font, "No networks found. Retry?", white);
            SDL_Texture *title_tex = SDL_CreateTextureFromSurface(renderer, title_surf);
            SDL_Rect title_rect = {left_margin, top_margin + 20, title_surf->w, title_surf->h};
            SDL_RenderCopy(renderer, title_tex, NULL, &title_rect);
            SDL_FreeSurface(title_surf);
            SDL_DestroyTexture(title_tex);

            // add back and retry button
            render_button(renderer, font, &back_button);
            render_button(renderer, font, &retry_button);

            return -1;
        }

        SDL_Surface *title_surf = TTF_RenderText_Solid(font, "Select Wi-Fi network:", white);
        SDL_Texture *title_tex = SDL_CreateTextureFromSurface(renderer, title_surf);
        SDL_Rect title_rect = {left_margin, top_margin, title_surf->w, title_surf->h};
        SDL_RenderCopy(renderer, title_tex, NULL, &title_rect);
        SDL_FreeSurface(title_surf);
        SDL_DestroyTexture(title_tex);

        int mx, my;
        SDL_GetMouseState(&mx, &my);

        for (int i = 0; i < net_count; i++)
        {
            SDL_Rect r = {left_margin, top_margin + select_text_height + i * 40, 320, 30};
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

            char network_name[32];
            clip_string(network_name, networks[i].ssid, 32);
            
            SDL_Surface *s = TTF_RenderText_Solid(font, network_name, (SDL_Color){255,255,255,255});
            SDL_Texture *t = SDL_CreateTextureFromSurface(renderer, s);
            SDL_Rect dst = {r.x + 10, r.y + (r.h - s->h)/2, s->w, s->h};
            SDL_RenderCopy(renderer, t, NULL, &dst);
            SDL_FreeSurface(s);
            SDL_DestroyTexture(t);
        }

        // add back and retry button
        render_button(renderer, font, &back_button);

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

                if (button_is_pressed(back_button, mx, my))
                {
                    current_screen = SCREEN_MAIN;
                    has_attempted_connection = 0;
                    select_network_index = -1;
                    networks_ready = 0;
                    ready_for_password = 0;
                    return -1;
                }

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

        SDL_Delay(16); // just under 60 frames per second
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

int mouse_over_button(Button *btn, int mx, int my)
{
    return mx >= btn->x && mx <= btn->x + btn->w && my >= btn->y && my <= btn->y + btn->h;
}

void kill_camera_users() 
{
    libusb_context *ctx;
    libusb_device **list;
    ssize_t cnt;
    libusb_init(&ctx);
    cnt = libusb_get_device_list(ctx, &list);

    for (ssize_t i = 0; i < cnt; i++)
    {
        struct libusb_device_descriptor desc;
        libusb_get_device_descriptor(list[i], &desc);
        if (desc.idVendor == 0x04b0 && desc.idProduct == 0x043a)
        {
            uint8_t bus = libusb_get_bus_number(list[i]);
            uint8_t addr = libusb_get_device_address(list[i]);
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "fuser -k /dev/bus/usb/%03d/%03d", bus, addr);
            system(cmd);
        }
    }

    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
}

void handle_sigint(int sig)
{
    _log("Logging signal interrupt: %i", sig);
    stop_requested = 1;
    kill_camera_users();
}

void setup_config_buttons(int screen_w, int screen_h, Button *back_button)
{
    int margin = screen_w / 50;
    int spacing = screen_w / 50;
    int btn_w = (screen_w - margin*2 - spacing) / 2;
    int btn_h = screen_h / 5;
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

void setup_retry_button(int screen_w, int screen_h, Button *retry_button)
{
    int margin = screen_w / 50;
    int spacing = screen_w / 50;
    int btn_w = (screen_w - margin*2 - spacing) / 2;
    int btn_h = screen_h / 5;
    int y = screen_h - btn_h - margin;

    retry_button->x = margin + btn_w + spacing;
    retry_button->y = y;
    retry_button->w = btn_w;
    retry_button->h = btn_h;
    retry_button->label = "Retry";
}

void setup_buttons(int screen_w, int screen_h, Button buttons[])
{
    int margin = screen_w / 50;
    int spacing = screen_w / 50;
    int btn_w = (screen_w - margin*2 - spacing) / 2;
    int btn_h = screen_h / 5;
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

    int start_x = 10;
    int y_pos = 10;

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

    int margin = 20;

    char imported_text[64];
    snprintf(imported_text, sizeof(imported_text), "%i image%s imported", image_status->imported, image_status->imported == 1 ? "" : "s");
    char uploaded_text[64];
    snprintf(uploaded_text, sizeof(uploaded_text), "%i image%s sent to server", image_status->uploaded, image_status->uploaded == 1 ? "" : "s");

    int y_offset = 100;
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

int render_password_prompt(SDL_Renderer *renderer, TTF_Font *font, const char *ssid, char *password, int max_len)
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

        char masked[MAX_PASSWORD];
        int len = strlen(password);
        if (len > 0) memset(masked, '*', len);
        masked[len] = 0;
        render_text(renderer, font, masked, 50, 100);

        SDL_RenderPresent(renderer);

        while (SDL_PollEvent(&e))
        {
            if (e.type == SDL_QUIT) stop_requested = 1;

            if (e.type == SDL_TEXTINPUT)
                strncat(password, e.text.text, max_len - strlen(password) - 1);

            if (e.type == SDL_KEYDOWN)
            {
                if (e.key.keysym.sym == SDLK_RETURN) done = 1;
                if (e.key.keysym.sym == SDLK_BACKSPACE)
                {
                    int l = strlen(password);
                    if (l > 0) password[l-1] = 0;
                }
            }
        }
        SDL_Delay(16);
    }

    SDL_StopTextInput();
    return done;
}
