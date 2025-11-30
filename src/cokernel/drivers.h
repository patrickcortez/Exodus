#ifndef COKERNEL_DRIVERS_H
#define COKERNEL_DRIVERS_H

#include <stddef.h>

typedef struct {
    const char* name;
    int (*init)(void);
    int (*read)(void* buf, size_t size);
    int (*write)(const void* buf, size_t size);
} ck_driver_t;

void ck_drivers_init(void);
int ck_register_driver(ck_driver_t* driver);
ck_driver_t* ck_get_driver(const char* name);

#endif // COKERNEL_DRIVERS_H
