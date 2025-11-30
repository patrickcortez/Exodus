// File: eat_mem.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main() {
    // Allocate 600 MB, which is more than the default 512MB cgroup limit
    size_t mem_to_eat = 600 * 1024 * 1024;
    printf("Attempting to allocate and use %zu MB of memory...\n", mem_to_eat / 1024 / 1024);

    char *buffer = (char*)malloc(mem_to_eat);
    if (!buffer) {
        perror("malloc failed");
        return 1;
    }

    printf("Allocation successful. Now writing to memory to ensure pages are resident...\n");
    // Write to the memory to make sure the kernel actually allocates physical pages
    memset(buffer, 'Z', mem_to_eat);

    printf("Memory is in use. I will now sleep for 30 seconds. Check 'lsmem' in another terminal.\n");
    sleep(30);

    printf("Done. Freeing memory.\n");
    free(buffer);

    return 0;
}