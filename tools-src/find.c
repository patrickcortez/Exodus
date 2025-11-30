/* tools/find.c
 *
 * A simple tool to find files recursively.
 * Supports basic wildcard matching with '*' and '?'.
 * 
 * Build: 
 * gcc -o find find.c
 */

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// A simple wildcard match function
// Returns 1 if 'str' matches 'pattern', 0 otherwise.
int wildcard_match(const char *pattern, const char *str) {
    while (*str) {
        if (*pattern == '*') {
            // Skip consecutive '*'
            while (*pattern == '*') pattern++;
            if (!*pattern) return 1; // '*' at the end matches everything
            while (*str) {
                if (wildcard_match(pattern, str)) return 1;
                str++;
            }
            return 0;
        } else if (*pattern == '?' || *pattern == *str) {
            pattern++;
            str++;
        } else {
            return 0;
        }
    }
    // End of string, check if pattern is also at the end or just trailing '*'
    while (*pattern == '*') pattern++;
    return !*pattern;
}

// Recursively search for files matching a pattern
void find_files(const char *base_path, const char *pattern) {
    DIR *dir;
    struct dirent *entry;

    if (!(dir = opendir(base_path))) {
        // Silently ignore directories we can't open
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        // Skip '.' and '..' directories
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", base_path, entry->d_name);

        struct stat path_stat;
        if (stat(path, &path_stat) != 0) {
            continue; // Skip files we can't stat
        }

        // If it's a directory, recurse into it
        if (S_ISDIR(path_stat.st_mode)) {
            find_files(path, pattern);
        } else {
            // It's a file, check if its name matches the pattern
            if (wildcard_match(pattern, entry->d_name)) {
                printf("%s\n", path);
            }
        }
    }
    closedir(dir);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: find <pattern>\n");
        fprintf(stderr, "Example: find \"*.txt\"\n");
        return 1;
    }

    const char *pattern = argv[1];
    find_files(".", pattern); // Start searching from the current directory "."

    return 0;
}