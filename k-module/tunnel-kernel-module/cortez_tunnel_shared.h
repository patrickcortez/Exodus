#ifndef CORTEZ_TUNNEL_SHARED_H
#define CORTEZ_TUNNEL_SHARED_H

#include <linux/ioctl.h>

// Define the magic number for our ioctl commands
#define CORTEZ_TUNNEL_MAGIC 't'

// Structure to pass from user to kernel when creating a tunnel
typedef struct {
    char name[32];
    unsigned long size;
} tunnel_create_t;

// Define the ioctl commands
#define TUNNEL_CREATE _IOW(CORTEZ_TUNNEL_MAGIC, 1, tunnel_create_t *)
#define TUNNEL_CONNECT _IOW(CORTEZ_TUNNEL_MAGIC, 2, char *)

#endif // CORTEZ_TUNNEL_SHARED_H
