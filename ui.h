#include <sys/types.h>
#include <sys/wait.h>
#include <libusb-1.0/libusb.h>
#include <unistd.h>
#include "buttons.h"

#define MAX_NETWORKS 5
#define MAX_PASSWORD 128
#define MAX_CMD 512

typedef struct
{
    int font_size;
    int ui_padding_left;
    int ui_padding_top;
    int ui_top_bar_height;
} UI_parameters;

UI_parameters ui_parameters;

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
volatile int clear_all_imports = 0;

pid_t conn_pid = 0;
int conn_status = -1;

typedef struct
{
    volatile int x;
    volatile int y;
} LastClick;

LastClick last_click = {0, 0};

typedef struct
{
    char ssid[128];
} WifiNetwork;

WifiNetwork networks[MAX_NETWORKS];

typedef enum
{
    CAMERA_STATUS_NO_CAMERA,
    CAMERA_STATUS_WAITING,
    CAMERA_STATUS_IMPORTING,
    CAMERA_STATUS_UPLOADING,
    CAMERA_STATUS_IMPORT_ONLY
} IMAGE_STATUS;

typedef struct
{
    int imported;
    int uploaded;
    IMAGE_STATUS status; // 0 = waiting, 1 = importing, 2 = uploading, 3 = No internet, import only
} ImageStatus;



Screen current_screen = SCREEN_MAIN;

int number_dots_for_loading_screen = 1;
int screen_refresh_count = 0;

int navigation_button_is_pressed(Button button, int mx, int my)
{
    // checks if the click event occured inside the boundaries of the button. Does not know Z-index.
    return mx >= button.x && mx <= button.x + button.w && my >= button.y && my <= button.y + button.h;
}

void render_wrappable_text(SDL_Renderer *renderer, TTF_Font *font, const char *text, int x, int y, int wrap_width)
{
    SDL_Surface *surf = TTF_RenderText_Blended_Wrapped(font, text, ui_colors.white, wrap_width);
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_Rect dst = {x, y, surf->w, surf->h};
    SDL_RenderCopy(renderer, tex, NULL, &dst);
    SDL_FreeSurface(surf);
    SDL_DestroyTexture(tex);
}

void render_colored_text(SDL_Renderer *renderer, TTF_Font *font, const char *text, int x, int y, SDL_Color color) 
{
    SDL_Surface *surface = TTF_RenderUTF8_Solid(font, text, color);
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_Rect dst = {x, y, surface->w, surface->h};
    SDL_RenderCopy(renderer, texture, NULL, &dst);
    SDL_FreeSurface(surface);
    SDL_DestroyTexture(texture);
}

void render_text(SDL_Renderer *renderer, TTF_Font *font, const char *text, int x, int y) 
{
    SDL_Surface *surface = TTF_RenderUTF8_Solid(font, text, ui_colors.white);
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
    render_text(renderer, font, loading_statement, ui_parameters.ui_padding_left, ui_parameters.ui_top_bar_height);
}

int list_networks(int max)
{
    // reset array to empty
    memset(networks, 0, sizeof(networks));

    FILE *fp = popen("nmcli -t -f SSID dev wifi", "r");
    if (!fp)
    {
        _log(LOG_ERROR, "Can not open file tto read Wifi status.");
        return 0;
    }

    _log(LOG_GENERAL, "Attempting to read Wifi information...");

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
                _log(LOG_GENERAL, "Adding network %s to network list.", line);
                strncpy(networks[count].ssid, line, sizeof(networks[count].ssid));
                networks[count].ssid[sizeof(networks[count].ssid)-1] = 0;
                count++;
            }
            else
            {
                _log(LOG_GENERAL, "Declining to add network %s to network list because it is a duplicate.", line);
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

    SDL_Surface *surface = TTF_RenderText_Solid(font, btn.label, ui_colors.white);
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
    const char *status_text;

    switch (camera_found)
    {
        case -1:
            status_text = "Camera detected - no communication";
            font_color = ui_colors.yellow;
            break;
        case 1:
            status_text = "Camera detected";
            font_color = ui_colors.green;
            break;
        default:
            status_text = "No camera detected";
            font_color = ui_colors.red;
            break;
    }

    render_colored_text(renderer, font, status_text, ui_parameters.ui_padding_left, ui_parameters.ui_padding_top, font_color);
}

void render_connection_status(SDL_Renderer *renderer, TTF_Font *font)
{
    SDL_Color font_color = internet_up ? ui_colors.green : ui_colors.red;
    const char *status_text = internet_up ? "Internet connected" : "No internet";

    int screen_width, screen_height;
    SDL_GetRendererOutputSize(renderer, &screen_width, &screen_height);

    SDL_Surface *surface = TTF_RenderText_Solid(font, status_text, font_color);
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);

    int bar_size = (int)(surface->h * 0.66);
    int spacing = 10;
    int total_width = surface->w + spacing + bar_size;
    int start_x = screen_width - total_width - ui_parameters.ui_padding_left;

    render_colored_text(renderer, font, status_text, start_x, ui_parameters.ui_padding_top, font_color);

    int text_center_y = ui_parameters.ui_padding_top + surface->h / 2;
    int bar_y = text_center_y - bar_size / 2;
    render_signal_indicator(renderer, start_x + surface->w + spacing, bar_y, bar_size, bar_size, link_strength_value);

    SDL_FreeSurface(surface);
    SDL_DestroyTexture(texture);
}

void render_status_box(SDL_Renderer *renderer, TTF_Font *font, ImageStatus *image_status)
{
    char imported_text[64];
    snprintf(imported_text, sizeof(imported_text), "%i image%s imported", image_status->imported, image_status->imported == 1 ? "" : "s");
    char uploaded_text[64];
    snprintf(uploaded_text, sizeof(uploaded_text), "%i image%s sent to server", image_status->uploaded, image_status->uploaded == 1 ? "" : "s");

    int y_offset = ui_parameters.ui_top_bar_height;
    render_text(renderer, font, imported_text, ui_parameters.ui_padding_left, y_offset);
    y_offset += ui_parameters.font_size + (ui_parameters.font_size / 25);

    render_text(renderer, font, uploaded_text, ui_parameters.ui_padding_left, y_offset);
    y_offset += ui_parameters.font_size + (ui_parameters.font_size / 25);

    SDL_Color color;
    char status_str[64];
    switch (image_status->status)
    {
        case CAMERA_STATUS_NO_CAMERA:
            strcpy(status_str, "Please attach or power on a camera");
            color = ui_colors.red;
            break;
        case CAMERA_STATUS_WAITING:
            strcpy(status_str, "Waiting for images");
            color = ui_colors.white;
            break;
        case CAMERA_STATUS_IMPORTING:
            strcpy(status_str, "Importing images");
            color = ui_colors.green;
            break;
        case CAMERA_STATUS_UPLOADING:
            strcpy(status_str, "Uploading images");
            color = ui_colors.green;
            break;
        case CAMERA_STATUS_IMPORT_ONLY:
            strcpy(status_str, "Importing images only - no internet");
            color = ui_colors.yellow;
            break;
    }
    
    create_text_with_dynamic_elipsis(status_str, 64);
    render_colored_text(renderer, font, status_str, ui_parameters.ui_padding_left, y_offset, color);
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
        render_text(renderer, font, loading_statement, ui_parameters.ui_padding_left, ui_parameters.ui_top_bar_height);
        render_button(renderer, font, navigation_buttons.back);
        render_button(renderer, font, navigation_buttons.retry);
    }
    else
    {
        strcpy(loading_statement, "Network connection successful.");
        render_text(renderer, font, loading_statement, ui_parameters.ui_padding_left, ui_parameters.ui_top_bar_height);
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
    SDL_Surface *title_surf = TTF_RenderText_Solid(font, "No networks found. Retry?", ui_colors.white);
    SDL_Texture *title_tex = SDL_CreateTextureFromSurface(renderer, title_surf);
    SDL_Rect title_rect = {ui_parameters.ui_padding_left, ui_parameters.ui_top_bar_height, title_surf->w, title_surf->h};

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

    int select_text_height = ui_parameters.font_size + (ui_parameters.font_size / 20);

    SDL_Surface *title_surf = TTF_RenderText_Solid(font, "Select Wi-Fi network:", ui_colors.white);
    SDL_Texture *title_tex = SDL_CreateTextureFromSurface(renderer, title_surf);
    SDL_Rect title_rect = {ui_parameters.ui_padding_left, ui_parameters.ui_top_bar_height, title_surf->w, title_surf->h};
    SDL_RenderCopy(renderer, title_tex, NULL, &title_rect);
    SDL_FreeSurface(title_surf);
    SDL_DestroyTexture(title_tex);

    for (int i = 0; i < net_count; i++)
    {
        SDL_Rect r = {ui_parameters.ui_padding_left, (ui_parameters.font_size / 2) + (ui_parameters.ui_top_bar_height + select_text_height) + i * 2 * select_text_height, (ui_parameters.ui_padding_left * 48), ui_parameters.font_size * 1.5};

        SDL_SetRenderDrawColor(renderer, 40, 40, 40, 255);
        SDL_RenderFillRect(renderer, &r);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderDrawRect(renderer, &r);

        char network_name[32];
        clip_string(network_name, networks[i].ssid, 32);
        SDL_Surface *s = TTF_RenderText_Solid(font, network_name, ui_colors.white);
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

    SDL_Surface *title_surf = TTF_RenderText_Solid(font, loading_statement, ui_colors.white);
    SDL_Texture *title_tex = SDL_CreateTextureFromSurface(renderer, title_surf);
    SDL_Rect title_rect = {ui_parameters.ui_padding_left, ui_parameters.ui_top_bar_height, title_surf->w, title_surf->h};

    SDL_RenderCopy(renderer, title_tex, NULL, &title_rect);
    SDL_FreeSurface(title_surf);
    SDL_DestroyTexture(title_tex);
    SDL_RenderPresent(renderer);

    if (!has_attempted_connection)
    {
        _log(LOG_GENERAL, "Attempting to connect to network...");
        network_connect_complete_status = 0;
        network_connect_complete = 0;
        has_attempted_connection = 1;
        conn_pid = fork();
        if (conn_pid == 0)
        {
            // call the existing blocking function in the child
            _log(LOG_GENERAL, "Connection attempt initiated.");
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
                _log(LOG_GENERAL, "Network connection failed: %d", conn_status);
                network_connect_complete_status = -1;
            }
            else
            {
                _log(LOG_ERROR, "Network connected successfully");
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

void render_clear_imports_confirmation_screen(SDL_Renderer * renderer, TTF_Font * font, Navigation_buttons navigation_buttons)
{
    char confirmation_text[] = "Are you sure you want to clear all imports?";
    render_text(renderer, font, confirmation_text, ui_parameters.ui_padding_left, ui_parameters.ui_top_bar_height + (ui_parameters.font_size / 25));

    int base_height = (2 * ui_parameters.ui_top_bar_height) + ui_parameters.font_size + 2 * (ui_parameters.font_size / 25);

    char warning_text[] = "WARNING - click 'Clear all' to:";
    render_text(renderer, font, warning_text, ui_parameters.ui_padding_left, base_height);

    const char *warning_instructions[] = {
        "- Delete all imported images from this device;",
        "- Delete track file of uploaded images;",
        "- Delete logs associated with this session."
    };

    int offset_top = base_height + ui_parameters.font_size + (ui_parameters.font_size / 25);
    int number_entries = sizeof(warning_instructions)/sizeof(warning_instructions[0]);
    int i = 0;

    for (i = 0; i < number_entries; i++)
    {
        offset_top = base_height + ((i + 1) * ui_parameters.font_size) + (1 + i) * (ui_parameters.font_size / 25);
        render_text(renderer, font, warning_instructions[i], ui_parameters.ui_padding_left * 2, offset_top);
    }

    i++;
    offset_top = base_height + ((i + 1) * ui_parameters.font_size) + (1 + i) * (ui_parameters.font_size / 25);

    char final_warning_text[] = "This action can not be undone. Ensure your images are retained off device before deleting. If a camera is still attached, or becomes reattached, any images will be re-imported and upload.";
    render_wrappable_text(renderer, font, final_warning_text, ui_parameters.ui_padding_left, offset_top, ui_parameters.ui_padding_left * 48);

    render_button(renderer, font, navigation_buttons.back);
    render_button(renderer, font, navigation_buttons.confirm_clear_imports);
}

void render_clear_imports_complete_screen(SDL_Renderer * renderer, TTF_Font * font, Navigation_buttons navigation_buttons)
{
    if (clear_all_imports)
    {
        clear_all_imports = 0;
        delete_images_in_import_folder();
        clear_track_file();
        clear_log_file();
        _log(LOG_GENERAL, "Log file cleared by user.");
    }

    char confirmation_text[] = "Imported images have been deleted from device.";
    render_text(renderer, font, confirmation_text, ui_parameters.ui_padding_left, ui_parameters.ui_top_bar_height + (ui_parameters.font_size / 25));
    
    render_button(renderer, font, navigation_buttons.back);
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

        case SCREEN_CLEAR_IMPORTS_CONFIRMATION:
            render_clear_imports_confirmation_screen(renderer, font, navigation_buttons);
            break;

        case SCREEN_CLEAR_IMPORTS_COMPLETE:
            render_clear_imports_complete_screen(renderer, font, navigation_buttons);
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

                case SCREEN_CLEAR_IMPORTS_CONFIRMATION:
                    if (navigation_button_is_pressed(navigation_buttons.back, last_click.x, last_click.y))
                    {
                        clear_all_imports = 0;
                        current_screen = navigation_buttons.back.target_screen;
                    }
                    else if (navigation_button_is_pressed(navigation_buttons.confirm_clear_imports, last_click.x, last_click.y))
                    {
                        clear_all_imports = 1;
                        current_screen = navigation_buttons.confirm_clear_imports.target_screen;
                    }
                    break;

                case SCREEN_CLEAR_IMPORTS_COMPLETE:
                    if (navigation_button_is_pressed(navigation_buttons.back, last_click.x, last_click.y))
                    {
                        clear_all_imports = 0;
                        current_screen = navigation_buttons.back.target_screen;
                    }
                    break;
            }
        }
    }
}

void run_UI(ImageStatus *image_status, int full_screen_mode)
{
    SDL_Window *window;
    SDL_Renderer *renderer;
    
    if (full_screen_mode)
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

    ui_parameters.font_size = screen_height / 20;
    ui_parameters.ui_padding_top = screen_width / 50; // font size + 2%
    ui_parameters.ui_padding_left = screen_width / 50; // 2%
    ui_parameters.ui_top_bar_height = ui_parameters.ui_padding_top + ui_parameters.font_size + (ui_parameters.font_size / 4);

    TTF_Font *font = TTF_OpenFont("Rubik/Rubik-VariableFont_wght.ttf", ui_parameters.font_size);
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