#include "drivers.h"
#include <stdio.h>
#include <string.h>

#define MAX_DRIVERS 10

static ck_driver_t* g_drivers[MAX_DRIVERS];
static int g_driver_count = 0;

void ck_drivers_init(void) {
    g_driver_count = 0;
    printf("[CoKernel] Driver subsystem initialized.\n");
}

int ck_register_driver(ck_driver_t* driver) {
    if (g_driver_count >= MAX_DRIVERS) return -1;
    g_drivers[g_driver_count++] = driver;
    printf("[CoKernel] Registered driver: %s\n", driver->name);
    if (driver->init) {
        return driver->init();
    }
    return 0;
}

ck_driver_t* ck_get_driver(const char* name) {
    for (int i = 0; i < g_driver_count; ++i) {
        if (strcmp(g_drivers[i]->name, name) == 0) {
            return g_drivers[i];
        }
    }
    return NULL;
}
