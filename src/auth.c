// src/auth.c
#define _GNU_SOURCE
#include "auth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <sodium.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <libgen.h>

#define MAX_USERNAME 128
#define MAX_MSG 256

/* ---------- File helpers ---------- */

static const char *get_fs_root(void) {
    static char root[PATH_MAX] = {0};
    if (root[0]) return root;

    char exec_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exec_path, sizeof(exec_path) - 1);
    if (len < 0) {
        getcwd(root, sizeof(root));
        return root;
    }
    exec_path[len] = '\0';

    strcpy(root, exec_path);
    char *slash = strrchr(root, '/');
    if (slash) *slash = '\0';  // Now dirname(exec_path)

    char *last_slash = strrchr(root, '/');
    if (last_slash && strcmp(last_slash + 1, "bin") == 0) {
        *last_slash = '\0';
    }

    if (root[0] == '\0') root[0] = '/';  // Ensure non-empty for mounted root
    return root;
}

static const char *get_data_dir(void) {
    static char dir[PATH_MAX] = {0};
    if (!dir[0]) snprintf(dir, sizeof(dir), "%s/data", get_fs_root());
    return dir;
}

static const char *get_profiles_path(void) {
    static char path[PATH_MAX] = {0};
    if (!path[0]) snprintf(path, sizeof(path), "%s/data/profiles.json", get_fs_root());
    return path;
}

static const char *get_home_base(void) {
    static char base[PATH_MAX] = {0};
    if (!base[0]) snprintf(base, sizeof(base), "%s/home", get_fs_root());
    return base;
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int ensure_profiles_file_exists(void) {
    struct stat st;
    if (stat(get_data_dir(), &st) == -1) {
        if (mkdir(get_data_dir(), 0755) != 0) return -1;
    }
    FILE *f = fopen(get_profiles_path(), "r");
    if (!f) {
        f = fopen(get_profiles_path(), "w");
        if (!f) return -1;
        fprintf(f, "[]\n");
        fclose(f);
    } else fclose(f);
    return 0;
}

static char *read_whole_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t r = fread(buf,1,(size_t)len,f);
    buf[r] = '\0';
    if (out_len) *out_len = r;
    fclose(f);
    return buf;
}

/* ---------- JSON helpers (naive but robust for our small format) ---------- */

/* If out_hash != NULL, writes stored passhash to it (space for crypto_pwhash_STRBYTES). */
static int profile_exists_internal(const char *username, char *out_hash) {
    size_t len;
    char *raw = read_whole_file(get_profiles_path(), &len);
    if (!raw) return 0;
    const char *p = raw;
    int found = 0;
    while ((p = strstr(p, "\"username\"")) != NULL) {
        const char *col = strchr(p, ':'); if (!col) break; col++;
        const char *q = strchr(col, '"'); if (!q) break; q++;
        const char *q2 = strchr(q, '"'); if (!q2) break;
        size_t uname_len = q2 - q;
        if (uname_len == strlen(username) && strncmp(q, username, uname_len) == 0) {
            /* found, find passhash */
            const char *ph = strstr(q2, "\"passhash\"");
            if (ph) {
                const char *phc = strchr(ph, ':'); if (!phc) break; phc++;
                const char *phs = strchr(phc, '"'); if (!phs) break; phs++;
                const char *phe = strchr(phs, '"'); if (!phe) break;
                size_t phlen = phe - phs;
                if (out_hash) {
size_t copy_n = phlen;
if (copy_n >= crypto_pwhash_STRBYTES) 
    copy_n = crypto_pwhash_STRBYTES - 1;
memcpy(out_hash, phs, copy_n);
out_hash[copy_n] = '\0';

                }
            }
            found = 1;
            break;
        }
        p = q2;
    }
    free(raw);
    return found;
}

static int add_profile_internal(const char *username, const char *passhash_str) {
    size_t len;
    char *raw = read_whole_file(get_profiles_path(), &len);
    if (!raw) return -1;
    /* Build object */
    char obj[1024];
char homepath[512];
snprintf(homepath, sizeof(homepath), "home/%s", username);
snprintf(obj, sizeof(obj),
         "{\"username\":\"%s\",\"passhash\":\"%s\",\"home\":\"%s\"}",
         username, passhash_str, homepath);

    FILE *f = fopen(get_profiles_path(), "w");
    if (!f) { free(raw); return -1; }
    /* trim leading whitespace */
    char *p = raw;
    while (*p && (*p==' '||*p=='\r'||*p=='\n'||*p=='\t')) p++;
    if (strcmp(p, "[]") == 0) {
        fprintf(f, "[%s]\n", obj);
    } else {
        char *last = strrchr(raw, ']');
        if (!last) {
            fprintf(f, "[%s]\n", obj);
        } else {
            size_t prefix = last - raw;
            fwrite(raw,1,prefix,f);
            fprintf(f, ",%s]\n", obj);
        }
    }
    fclose(f);
    free(raw);
    return 0;
}

/* ---------- libsodium password helpers ---------- */

int add_profile_secure(const char *username, const char *password) {
    if (profile_exists_internal(username, NULL)) return 1;
    if (sodium_init() < 0) return -1;
    char hash[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str(
            hash,
            password,
            strlen(password),
            crypto_pwhash_OPSLIMIT_INTERACTIVE,
            crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        return -1;
    }
    int r = add_profile_internal(username, hash);
    if (r != 0) return r;

    /* Create home/<username> and chmod 0700 so only the process owner can access it. */
    char basedir[512];
    snprintf(basedir, sizeof(basedir), "%s/home", get_fs_root());
    struct stat st;
    if (stat(basedir, &st) != 0) {
        if (mkdir(basedir, 0700) != 0) {
            /* Non-fatal for profile creation, but report error to caller */
            return -1;
        }
    }
    char homedir[512];
    snprintf(homedir, sizeof(homedir), "%s/%s", basedir, username);
    if (stat(homedir, &st) != 0) {
        if (mkdir(homedir, 0700) != 0) {
            return -1;
        }
    } else {
        /* ensure permissions are restrictive */
        chmod(homedir, 0700);
    }

    return 0;
}

/* Default "permissions" model is symbolic, your OS decides what 0700 means */
#define PERM_PRIVATE 0700

/* Standalone, platform-independent function */
int create_home_dir_and_chdir(const char *username, fs_ops_t *fs) {
    char homedir[512];

    if (!username || username[0] == '\0') {
        return -1; /* invalid username */
    }

    if (!fs || !fs->make_dir || !fs->path_exists || !fs->change_dir) {
        return -1; /* invalid ops */
    }

    char home_base[PATH_MAX];
    snprintf(home_base, sizeof(home_base), "%s/home", get_fs_root());
    if (!fs->path_exists(home_base)) { 
        if (fs->make_dir(home_base, PERM_PRIVATE) != 0) {
            return -1;
        }
    }

    /* ensure user home exists */
    snprintf(homedir, sizeof(homedir), "%s/%s", home_base, username);
    if (!fs->path_exists(homedir)) {
        if (fs->make_dir(homedir, PERM_PRIVATE) != 0) {
            return -1;
        }
    }

    /* enforce perms if supported */
    if (fs->set_perms) {
        fs->set_perms(homedir, PERM_PRIVATE);
    }

    /* switch to user home */
    if (fs->change_dir(homedir) != 0) {
        return -1;
    }

    return 0;
}


int verify_credentials_secure(const char *username, const char *password) {
    if (sodium_init() < 0) return 0;
    char stored[crypto_pwhash_STRBYTES];
    if (!profile_exists_internal(username, stored)) return 0;
    if (crypto_pwhash_str_verify(stored, password, strlen(password)) == 0) return 1;
    return 0;
}

/* POSIX implementation of fs_ops_t */
static int posix_make_dir(const char *path, unsigned int perms) {
    return mkdir(path, perms);
}
static int posix_path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}
static int posix_set_perms(const char *path, unsigned int perms) {
    return chmod(path, perms);
}
static int posix_change_dir(const char *path) {
    return chdir(path);
}

static fs_ops_t posix_fs = {
    .make_dir   = posix_make_dir,
    .path_exists= posix_path_exists,
    .set_perms  = posix_set_perms,
    .change_dir = posix_change_dir
};




/* ---------- SDL login modal ---------- */

/* small helper to draw text */
static SDL_Texture *render_text(SDL_Renderer *rend, TTF_Font *font, const char *txt) {
    SDL_Color white = {255,255,255,255};
    SDL_Surface *s = TTF_RenderUTF8_Blended(font, txt, white);
    if (!s) return NULL;
    SDL_Texture *t = SDL_CreateTextureFromSurface(rend, s);
    SDL_FreeSurface(s);
    return t;
}

/* draw a filled rounded-ish box (simple rect for now) */
static void draw_box(SDL_Renderer *rend, int x, int y, int w, int h) {
    SDL_Rect r = {x,y,w,h};
    SDL_SetRenderDrawColor(rend, 28,28,28, 255);
    SDL_RenderFillRect(rend, &r);
    SDL_SetRenderDrawColor(rend, 100,100,100,255);
    SDL_Rect border = {x, y, w, h};
    SDL_RenderDrawRect(rend, &border);
}

int get_home_for_user(const char *username, char *out_home, size_t out_sz) {
    size_t len;
    char *raw = read_whole_file(get_profiles_path(), &len);
    if (!raw) return -1;
    const char *p = raw;
    int found = 0;
    while ((p = strstr(p, "\"username\"")) != NULL) {
        const char *col = strchr(p, ':'); if (!col) break; col++;
        const char *q = strchr(col, '"'); if (!q) break; q++;
        const char *q2 = strchr(q, '"'); if (!q2) break;
        size_t uname_len = q2 - q;
        if (uname_len == strlen(username) && strncmp(q, username, uname_len) == 0) {
            const char *hm = strstr(q2, "\"home\"");
            if (hm) {
                const char *hcol = strchr(hm, ':'); if (!hcol) break; hcol++;
                const char *hs = strchr(hcol, '"'); if (!hs) break; hs++;
                const char *he = strchr(hs, '"'); if (!he) break;
                size_t hlen = he - hs;
                size_t cp = hlen < out_sz-1 ? hlen : out_sz-1;
                memcpy(out_home, hs, cp);
                out_home[cp] = '\0';
                found = 1;
            }
            break;
        }
        p = q2;
    }
    free(raw);
    return found ? 0 : -1;
}


int secure_path_for_user(const char *username, const char *requested_path,
                         char *out_resolved, size_t out_sz) {
    char home[512];
    if (get_home_for_user(username, home, sizeof(home)) != 0) return -1;

    char home_abs[PATH_MAX]; 
    snprintf(home_abs, sizeof(home_abs), "%s/%s", get_fs_root(), out_resolved);
    char combined[PATH_MAX];
    if (requested_path[0] == '/') {
        /* remove leading slash to interpret as inside home */
        snprintf(combined, sizeof(combined), "%s%s", home_abs, requested_path);
    } else {
        snprintf(combined, sizeof(combined), "%s/%s", home_abs, requested_path);
    }

    /* Resolve realpath (resolve symlinks) */
    char resolved[PATH_MAX];
    if (!realpath(combined, resolved)) return -1;

    /* Resolve realpath of home */
    char resolved_home[PATH_MAX];
    if (!realpath(home_abs, resolved_home)) return -1;

    size_t home_len = strlen(resolved_home);
    /* Ensure resolved path starts with resolved_home + '/' OR exactly equals resolved_home */
    if (strncmp(resolved, resolved_home, home_len) != 0) return -1;
    if (resolved[home_len] != '\0' && resolved[home_len] != '/') return -1;

    /* success: copy */
    if (strlen(resolved) + 1 > out_sz) return -1;
    strcpy(out_resolved, resolved);
    return 0;
}

/* Main modal: returns malloc'd username or NULL */
char *auth_sdl_login(SDL_Renderer *rend, TTF_Font *font, int win_w, int win_h) {
    if (ensure_profiles_file_exists() != 0) return NULL;
    if (sodium_init() < 0) return NULL;

    const int box_w = win_w * 2 / 5;
    const int box_h = 220;
    const int box_x = (win_w - box_w) / 2;
    const int box_y = (win_h - box_h) / 2;

    char username[MAX_USERNAME] = {0};
    char password[256] = {0};
    int focus = 0; /* 0=user,1=pass,2=continue,3=create */
    int uname_pos = 0, pass_pos = 0;
    char message[MAX_MSG] = {0};

    SDL_StartTextInput();
    SDL_Event ev;
    int running = 1;
    while (running) {
        /* draw */
        SDL_SetRenderDrawColor(rend, 0,0,0,255);
        SDL_RenderClear(rend);

        /* semi-opaque overlay */
        SDL_SetRenderDrawBlendMode(rend, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(rend, 0,0,0,160);
        SDL_Rect ov = {0,0,win_w,win_h};
        SDL_RenderFillRect(rend, &ov);

        draw_box(rend, box_x, box_y, box_w, box_h);
        /* title */
        SDL_Texture *title_t = render_text(rend, font, "Login to Cortez Terminal");
        if (title_t) {
            int tw, th; SDL_QueryTexture(title_t,NULL,NULL,&tw,&th);
            SDL_Rect d = {box_x + 12, box_y + 8, tw, th};
            SDL_RenderCopy(rend, title_t, NULL, &d);
            SDL_DestroyTexture(title_t);
        }

        /* labels and fields */
        SDL_Texture *u_label = render_text(rend,font,"Username:");
        SDL_Texture *p_label = render_text(rend,font,"Password:");
        if (u_label) { int tw,th; SDL_QueryTexture(u_label,NULL,NULL,&tw,&th); SDL_Rect d={box_x+12,box_y+36,tw,th}; SDL_RenderCopy(rend,u_label,NULL,&d); SDL_DestroyTexture(u_label); }
        if (p_label) { int tw,th; SDL_QueryTexture(p_label,NULL,NULL,&tw,&th); SDL_Rect d={box_x+12,box_y+76,tw,th}; SDL_RenderCopy(rend,p_label,NULL,&d); SDL_DestroyTexture(p_label); }

        /* field backgrounds */
        SDL_Rect ufield = {box_x+12+92, box_y+32, box_w-128, 24};
        SDL_Rect pfield = {box_x+12+92, box_y+72, box_w-128, 24};
        SDL_SetRenderDrawColor(rend, 18,18,18,255);
        SDL_RenderFillRect(rend, &ufield);
        SDL_RenderFillRect(rend, &pfield);
        SDL_SetRenderDrawColor(rend, 140,140,140,255);
        SDL_RenderDrawRect(rend, &ufield);
        SDL_RenderDrawRect(rend, &pfield);

        /* typed username and masked password */
        SDL_Texture *u_text = render_text(rend, font, username);
        char pmask[256]; for (size_t i=0;i<strlen(password);++i) pmask[i]='*'; pmask[strlen(password)]='\0';
        SDL_Texture *p_text = render_text(rend,font,pmask);
        if (u_text) { int tw,th; SDL_QueryTexture(u_text,NULL,NULL,&tw,&th); SDL_Rect d={ufield.x+4,ufield.y+1,tw,th}; SDL_RenderCopy(rend,u_text,NULL,&d); SDL_DestroyTexture(u_text); }
        if (p_text) { int tw,th; SDL_QueryTexture(p_text,NULL,NULL,&tw,&th); SDL_Rect d={pfield.x+4,pfield.y+1,tw,th}; SDL_RenderCopy(rend,p_text,NULL,&d); SDL_DestroyTexture(p_text); }

        /* buttons */
        const char *btn_continue = "< Continue >";
        const char *btn_create = "< Create >";
        SDL_Texture *b1 = render_text(rend,font,btn_continue);
        SDL_Texture *b2 = render_text(rend,font,btn_create);
        int bx = box_x + 24;
        int by = box_y + 120;
        if (b1) { int tw,th; SDL_QueryTexture(b1,NULL,NULL,&tw,&th); SDL_Rect d={bx,by,tw,th}; if (focus==2) { SDL_SetRenderDrawColor(rend,60,60,60,255); SDL_Rect br={d.x-6,d.y-4,d.w+12,d.h+8}; SDL_RenderFillRect(rend,&br);} SDL_RenderCopy(rend,b1,NULL,&d); SDL_DestroyTexture(b1); }
        if (b2) { int tw,th; SDL_QueryTexture(b2,NULL,NULL,&tw,&th); SDL_Rect d={bx+160,by,tw,th}; if (focus==3) { SDL_SetRenderDrawColor(rend,60,60,60,255); SDL_Rect br={d.x-6,d.y-4,d.w+12,d.h+8}; SDL_RenderFillRect(rend,&br);} SDL_RenderCopy(rend,b2,NULL,&d); SDL_DestroyTexture(b2); }

        /* message area (red on errors) */
        if (message[0]) {
            SDL_Color red = {220,40,40,255};
            SDL_Surface *ms = TTF_RenderUTF8_Blended(font, message, red);
            if (ms) {
                SDL_Texture *mt = SDL_CreateTextureFromSurface(rend, ms);
                int tw = ms->w, th = ms->h;
                SDL_Rect md = {box_x+12, box_y+box_h-34, tw, th};
                SDL_RenderCopy(rend, mt, NULL, &md);
                SDL_DestroyTexture(mt);
                SDL_FreeSurface(ms);
            }
        }

        SDL_RenderPresent(rend);

        /* event handling - navigation and typing */
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { SDL_StopTextInput(); return NULL; }
            else if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode k = ev.key.keysym.sym;
                if (k == SDLK_TAB) focus = (focus + 1) % 4;
                else if (k == SDLK_UP) { if (focus>0) focus--; }
                else if (k == SDLK_DOWN) { if (focus<3) focus++; }
                else if (k == SDLK_LEFT) { if (focus>=2) focus = 2; }
                else if (k == SDLK_RIGHT) { if (focus>=2) focus = 3; }
                else if (k == SDLK_BACKSPACE) {
                    if (focus == 0 && uname_pos>0) { uname_pos--; username[uname_pos]='\0'; }
                    else if (focus==1 && pass_pos>0) { pass_pos--; password[pass_pos]='\0'; }
                } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    if (focus == 2) {
                        /* attempt login */
                        if (strlen(username)==0 || strlen(password)==0) {
                            snprintf(message, sizeof(message), "ERROR: username and password required");
                        } else if (verify_credentials_secure(username, password)) {
                            /* create home and chdir */
                            if (create_home_dir_and_chdir(username, &posix_fs) != 0) {
                                snprintf(message, sizeof(message), "ERROR: failed to chdir to home/%s", username);
                            } else {
                                char *ret = malloc(strlen(username)+1);
                                strcpy(ret, username);
                                SDL_StopTextInput();
                                return ret;
                            }
                        } else {
                            snprintf(message, sizeof(message), "ERROR: invalid username or password");
                            password[0]='\0'; pass_pos=0;
                        }
                    } else if (focus == 3) {
                        /* create account */
                        if (strlen(username)==0 || strlen(password)==0) {
                            snprintf(message, sizeof(message), "ERROR: username and password required");
                        } else {
                            int r = add_profile_secure(username, password);
                            if (r == 0) {
                                snprintf(message, sizeof(message), "Account created. Return to login and press Continue.");
                                password[0]='\0'; pass_pos=0;
                                focus = 0;
                            } else if (r == 1) {
                                snprintf(message, sizeof(message), "ERROR: username already taken");
                            } else {
                                snprintf(message, sizeof(message), "ERROR: failed to create account");
                            }
                        }
                    }
                }
            } else if (ev.type == SDL_TEXTINPUT) {
                const char *txt = ev.text.text;
                if (focus == 0) {
                    size_t cur = strlen(username);
                    size_t add = strlen(txt);
                    if (cur + add < sizeof(username)-1) {
                        strncat(username, txt, add);
                    }
                } else if (focus == 1) {
                    size_t cur = strlen(password);
                    size_t add = strlen(txt);
                    if (cur + add < sizeof(password)-1) {
                        strncat(password, txt, add);
                    }
                }
            }
        } /* end event loop */

        SDL_Delay(10);
    } /* end modal loop */

    SDL_StopTextInput();
    return NULL;
}
