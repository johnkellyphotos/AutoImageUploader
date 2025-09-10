#include <SDL2/SDL.h>

typedef struct
{
    SDL_Color white;
    SDL_Color green;
    SDL_Color red;
    SDL_Color yellow;
} UI_Colors;

UI_Colors ui_colors = {{255, 255, 255, 255}, {55, 255, 55, 255}, {255, 55, 55, 255}, {255, 255, 55, 255}};