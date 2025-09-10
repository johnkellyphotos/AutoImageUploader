#include "screen.h"

typedef struct
{
    int x, y, w, h;
    const char *label;
    Screen target_screen;
} Button;

typedef struct
{
    Button select_network;
    Button back;
    Button confirm_clear_imports;
    Button retry;
    Button clear_import;
} Navigation_buttons;


Navigation_buttons initialize_navigation_buttons( int screen_width, int screen_height)
{
    Navigation_buttons navigation_buttons;
    
    int margin = screen_width / 50;
    int spacing = screen_width / 50;
    int btn_w = (screen_width - margin*2 - spacing) / 2;
    int btn_h = screen_height / 5;
    int y = screen_height - btn_h - margin;

    // left side of screen navigation buttons
    navigation_buttons.select_network.x = margin;
    navigation_buttons.select_network.y = y;
    navigation_buttons.select_network.w = btn_w;
    navigation_buttons.select_network.h = btn_h;
    navigation_buttons.select_network.label = "Select a network";
    navigation_buttons.select_network.target_screen = SCREEN_NETWORK_CONFIG;

    navigation_buttons.back.x = margin;
    navigation_buttons.back.y = y;
    navigation_buttons.back.w = btn_w;
    navigation_buttons.back.h = btn_h;
    navigation_buttons.back.label = "Back";
    navigation_buttons.back.target_screen = SCREEN_MAIN;

    //right side of screen navigation buttons
    navigation_buttons.clear_import.x = margin + btn_w + spacing;
    navigation_buttons.clear_import.y = y;
    navigation_buttons.clear_import.w = btn_w;
    navigation_buttons.clear_import.h = btn_h;
    navigation_buttons.clear_import.label = "Clear imports";
    navigation_buttons.clear_import.target_screen = SCREEN_CLEAR_IMPORTS_CONFIRMATION;

    navigation_buttons.retry.x = margin + btn_w + spacing;
    navigation_buttons.retry.y = y;
    navigation_buttons.retry.w = btn_w;
    navigation_buttons.retry.h = btn_h;
    navigation_buttons.retry.label = "Retry";
    navigation_buttons.retry.target_screen = SCREEN_NETWORK_CONFIG;

    navigation_buttons.confirm_clear_imports.x = margin + btn_w + spacing;
    navigation_buttons.confirm_clear_imports.y = y;
    navigation_buttons.confirm_clear_imports.w = btn_w;
    navigation_buttons.confirm_clear_imports.h = btn_h;
    navigation_buttons.confirm_clear_imports.label = "Clear all";
    navigation_buttons.confirm_clear_imports.target_screen = SCREEN_CLEAR_IMPORTS_COMPLETE;

    return navigation_buttons;
}