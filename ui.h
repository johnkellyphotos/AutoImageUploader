#include <signal.h>

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