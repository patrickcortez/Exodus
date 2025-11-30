// src/auth.h
#ifndef CORTEZ_AUTH_H
#define CORTEZ_AUTH_H

#include <stddef.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

/* Filesystem ops abstraction shared between frontend and auth.c */
typedef struct {
    int (*make_dir)(const char *path, unsigned int perms);
    int (*path_exists)(const char *path);
    int (*set_perms)(const char *path, unsigned int perms);
    int (*change_dir)(const char *path);
} fs_ops_t;

/* Ensure profiles.json exists. Returns 0 on success. */
int ensure_profiles_file_exists(void);

/* Add profile (uses libsodium internally) - returns 0 on success, 1 if username exists, -1 on other error */
int add_profile_secure(const char *username, const char *password);

/* Verify credentials. Returns 1 if OK, 0 otherwise. */
int verify_credentials_secure(const char *username, const char *password);

/* Create home/<username> if missing and chdir() into it. Uses fs_ops_t backend. Returns 0 on success. */
int create_home_dir_and_chdir(const char *username, fs_ops_t *fs);

/* Get home path stored for user in profiles.json. Returns 0 on success. */
int get_home_for_user(const char *username, char *out_home, size_t out_sz);

/* Resolve a user-supplied path to an absolute path inside the user's home */
int secure_path_for_user(const char *username, const char *requested_path,
                         char *out_resolved, size_t out_sz);

/* SDL login modal (implemented in auth.c). Renderer/font/size provided by caller.
   Returns malloc'd username string on success (caller must free) or NULL on cancel/fail.
*/
char *auth_sdl_login(SDL_Renderer *rend, TTF_Font *font, int win_w, int win_h);

#endif // CORTEZ_AUTH_H
