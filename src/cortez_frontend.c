// src/cortez_frontend_crt.c

#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <math.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include "auth.h"
static char sticker_text[128] = "Cortez Tech .inc";
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_mixer.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* --- ASCII art (Loading Screen) --- */
static const char *ART_LINES[] = {
"                  ",
"          _____                   _______                   _____                _____                    _____                    _____          ",
"         /\\    \\                 /::\\    \\                 /\\    \\              /\\    \\                  /\\    \\                  /\\    \\         ",
"        /::\\    \\               /::::\\    \\               /::\\    \\            /::\\    \\                /::\\    \\                /::\\    \\        ",
"       /::::\\    \\             /::::::\\    \\             /::::\\    \\           \\:::\\    \\              /::::\\    \\               \\:::\\    \\       ",
"      /::::::\\    \\           /::::::::\\    \\           /::::::\\    \\           \\:::\\    \\            /::::::\\    \\               \\:::\\    \\      ",
"     /:::/\\:::\\    \\         /:::/~~\\:::\\    \\         /:::/\\:::\\    \\           \\:::\\    \\          /:::/\\:::\\    \\               \\:::\\    \\     ",
"    /:::/  \\:::\\    \\       /:::/    \\:::\\    \\       /:::/__\\:::\\    \\           \\:::\\    \\        /:::/__\\:::\\    \\               \\:::\\    \\    ",
"   /:::/    \\:::\\    \\     /:::/    / \\:::\\    \\     /::::\\   \\:::\\    \\          /::::\\    \\      /::::\\   \\:::\\    \\               \\:::\\    \\   ",
"  /:::/    / \\:::\\    \\   /:::/____/   \\:::\\____\\   /::::::\\   \\:::\\    \\        /::::::\\    \\    /::::::\\   \\:::\\    \\               \\:::\\    \\  ",
" /:::/    /   \\:::\\    \\ |:::|    |     |:::|    | /:::/\\:::\\   \\:::\\____\\      /:::/\\:::\\    \\  /:::/\\:::\\   \\:::\\    \\               \\:::\\    \\ ",
"/:::/____/     \\:::\\____\\|:::|____|     |:::|    |/:::/  \\:::\\   \\:::|    |    /:::/  \\:::\\____\\/:::/__\\:::\\   \\:::\\____\\_______________\\:::\\____| ",
"\\:::\\    \\      \\::/    / \\:::\\    \\   /:::/    / \\::/   |::::\\  /:::|____|   /:::/    \\::/    /\\:::\\   \\:::\\   \\::/    /\\::::::::::::::::::/    / ",
" \\:::\\    \\      \\/____/   \\:::\\    \\ /:::/    /   \\/____|:::::\\/:::/    /   /:::/    / \\/____/  \\:::\\   \\:::\\   \\/____/  \\::::::::::::::::/____/  ",
"  \\:::\\    \\                \\:::\\    /:::/    /          |:::::::::/    /   /:::/    /            \\:::\\   \\:::\\    \\       \\:::\\~~~~\\~~~~~~       ",
"   \\:::\\    \\                \\:::\\__/:::/    /           |::|\\::::/    /   /:::/    /              \\:::\\   \\:::\\____\\       \\:::\\    \\            ",
"    \\:::\\    \\                \\::::::::/    /            |::| \\::/____/    \\::/    /                \\:::\\   \\::/    /        \\:::\\    \\           ",
"     \\:::\\    \\                \\::::::/    /             |::|  ~|           \\/____/                  \\:::\\   \\/____/          \\:::\\    \\          ",
"      \\:::\\    \\                \\::::/    /              |::|   |                                     \\:::\\    \\               \\:::\\    \\         ",
"       \\:::\\____\\                \\::/____/               \\::|   |                                      \\:::\\____\\               \\:::\\____\\        ",
"        \\::/    /                 ~~                      \\:|   |                                       \\::/    /                \\::/    /        ",
"         \\/____/                                           \\|___|                                        \\/____/                  \\/____/         ",
NULL
};

#define FONT_SIZE 18
#define INPUT_HEIGHT 34
#define LINE_SPACING 6
/* ---- Auth forward declarations & filesystem adapter ---- */
#ifndef MAX_USERNAME
#define MAX_USERNAME 64
#endif
#ifndef MAX_MSG
#define MAX_MSG 256
#endif
SDL_Texture *corner_shadow = NULL;
SDL_Texture *side_glow_left = NULL;
SDL_Texture *side_glow_right = NULL;
int corner_sz = 160; 
int side_glow_w = 96; /* width of glow band on each side */
static Mix_Music *g_current_music = NULL;




static int is_in_bin_dir(void) {
    char exec_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exec_path, sizeof(exec_path) - 1);
    if (len < 0) return 0;
    exec_path[len] = '\0';

    char *slash = strrchr(exec_path, '/');
    if (!slash) return 0;
    *slash = '\0';

    char *last_slash = strrchr(exec_path, '/');
    if (last_slash && strcmp(last_slash + 1, "bin") == 0) return 1;
    return 0;
}

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
    if (slash) *slash = '\0';

    char *last_slash = strrchr(root, '/');
    if (last_slash && strcmp(last_slash + 1, "bin") == 0) {
        *last_slash = '\0';
    }

    if (root[0] == '\0') root[0] = '/';
    return root;
}

/* base64 helpers */
static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static size_t b64_decode(const char *in, unsigned char *out, size_t outcap) {
    size_t len = strlen(in), i=0,o=0;
    while (i < len) {
        int v[4] = {-1,-1,-1,-1};
        int k;
        for (k=0;k<4 && i < len;k++) {
            char c = in[i++];
            if (c=='=') { v[k] = -2; continue; }
            int val = b64_val(c);
            if (val < 0) { k--; continue; }
            v[k]=val;
        }
        if (v[0] < 0) break;
        uint32_t triple = ((v[0]&0x3F)<<18)|((v[1]&0x3F)<<12)|((v[2]&0x3F)<<6)|(v[3]&0x3F);
        if (v[2]==-2) { if (o+1<=outcap) out[o++]=(triple>>16)&0xFF; break; }
        else if (v[3]==-2) { if (o+2<=outcap) { out[o++]=(triple>>16)&0xFF; out[o++]=(triple>>8)&0xFF; } break; }
        else { if (o+3<=outcap) { out[o++]=(triple>>16)&0xFF; out[o++]=(triple>>8)&0xFF; out[o++]=triple&0xFF; } else break; }
    }
    return o;
}


/* strip common ANSI escape (CSI) sequences from a string in-place.
   Removes ESC [ ... <final> sequences and also drops stray \r characters. */
static void strip_ansi_sequences(char *s) {
    if (!s) return;
    char *dst = s;
    char *src = s;
    while (*src) {
        if (*src == '\x1b') {
            /* skip ESC */
            src++;
            /* if next is '[' it's a CSI sequence: skip until final byte (@-~) */
            if (*src == '[') {
                src++;
                while (*src && !(*src >= '@' && *src <= '~')) src++;
                if (*src) src++; /* consume final byte */
            } else {
                /* other ESC sequences: skip until a final byte */
                while (*src && !(*src >= '@' && *src <= '~')) src++;
                if (*src) src++;
            }
            continue;
        }
        /* drop carriage returns injected by PTY (we'll rely on '\n' for line breaks) */
        if (*src == '\r') { src++; continue; }
        *dst++ = *src++;
    }
    *dst = '\0';
}



/* POSIX-backed fs_ops for the mounted filesystem (placed in frontend as requested) */
static int posix_make_dir(const char *path, unsigned int perms) {
    if (mkdir(path, perms) != 0) {
        if (errno == EEXIST) return 0;
        return -1;
    }
    return 0;
}
static int posix_path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}
static int posix_set_perms(const char *path, unsigned int perms) {
    (void)perms;
    /* try chmod, ignore errors for non-POSIX filesystems */
    return chmod(path, perms);
}
static int posix_change_dir(const char *path) {
    return chdir(path);
}
static fs_ops_t posix_fs = {
    .make_dir = posix_make_dir,
    .path_exists = posix_path_exists,
    .set_perms = posix_set_perms,
    .change_dir = posix_change_dir
};



static SDL_Texture *render_text(SDL_Renderer *rend, TTF_Font *font, const char *txt) {
    if (!rend || !font || !txt) return NULL;
    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *s = TTF_RenderUTF8_Blended(font, txt, white);
    if (!s) return NULL;
    SDL_Texture *t = SDL_CreateTextureFromSurface(rend, s);
    SDL_FreeSurface(s);
    return t;
}


/* ------------------------------------------------------------------
   Modal popup drawn into the given renderer (no new SDL_Window).
   Returns malloc'd username on success (caller must free), or NULL.
   ------------------------------------------------------------------ */
static char *run_login_modal_on_renderer(SDL_Renderer *rend, TTF_Font *font,
                                         int win_w, int win_h)
{
    if (!rend || !font) return NULL;

    /* buffers */
    char username[MAX_USERNAME]; username[0]='\0';
    char password[256]; password[0]='\0';
    int focus = 0; /* 0=user,1=pass,2=continue,3=create */
    int uname_pos = 0, pass_pos = 0;
    char message[MAX_MSG]; message[0] = '\0';
    SDL_Event ev;

    SDL_StartTextInput();

    /* modal geometry */
    const int box_w = win_w * 2 / 5;
    const int box_h = 220;
    const int box_x = (win_w - box_w) / 2;
    const int box_y = (win_h - box_h) / 2;

    /* helper to render current frame of modal */
    auto_render_modal: {
        /* clear with current scene? We assume caller will have rendered background (CRT).
           To ensure overlay appears, we render a translucent overlay on top of whatever's already drawn.
           Caller should call this function right after painting the background CRT, before presenting.
        */

        /* semi-opaque overlay */
        SDL_SetRenderDrawBlendMode(rend, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(rend, 0,0,0,160);
        SDL_Rect ov = {0,0,win_w,win_h};
        SDL_RenderFillRect(rend, &ov);

        /* box background */
        SDL_SetRenderDrawBlendMode(rend, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(rend, 28,28,28,255);
        SDL_Rect box = {box_x, box_y, box_w, box_h};
        SDL_RenderFillRect(rend, &box);

        /* border */
        SDL_SetRenderDrawColor(rend, 100,100,100,255);
        SDL_Rect border = {box_x, box_y, box_w, box_h};
        SDL_RenderDrawRect(rend, &border);

        /* title */
        SDL_Texture *title_t = render_text(rend, font, "Login to Cortez Terminal");
        if (title_t) {
            int tw, th; SDL_QueryTexture(title_t, NULL, NULL, &tw, &th);
            SDL_Rect d = {box_x + 12, box_y + 8, tw, th};
            SDL_RenderCopy(rend, title_t, NULL, &d);
            SDL_DestroyTexture(title_t);
        }

        /* labels */
        SDL_Texture *u_label = render_text(rend, font, "Username:");
        SDL_Texture *p_label = render_text(rend, font, "Password:");
        if (u_label) { int tw,th; SDL_QueryTexture(u_label,NULL,NULL,&tw,&th); SDL_Rect d={box_x+12,box_y+36,tw,th}; SDL_RenderCopy(rend,u_label,NULL,&d); SDL_DestroyTexture(u_label); }
        if (p_label) { int tw,th; SDL_QueryTexture(p_label,NULL,NULL,&tw,&th); SDL_Rect d={box_x+12,box_y+76,tw,th}; SDL_RenderCopy(rend,p_label,NULL,&d); SDL_DestroyTexture(p_label); }

        /* input fields */
        SDL_Rect ufield = {box_x+12+92, box_y+32, box_w-128, 24};
        SDL_Rect pfield = {box_x+12+92, box_y+72, box_w-128, 24};
        SDL_SetRenderDrawColor(rend, 18,18,18,255);
        SDL_RenderFillRect(rend, &ufield);
        SDL_RenderFillRect(rend, &pfield);
        SDL_SetRenderDrawColor(rend, 140,140,140,255);
        SDL_RenderDrawRect(rend, &ufield);
        SDL_RenderDrawRect(rend, &pfield);

        /* typed text and masked password */
        SDL_Texture *u_text = render_text(rend, font, username);
        char pmask[256]; size_t pwlen = strlen(password);
        for (size_t i=0;i<pwlen && i<sizeof(pmask)-1;i++) pmask[i] = '*';
        pmask[pwlen < sizeof(pmask)-1 ? pwlen : sizeof(pmask)-1] = '\0';
        SDL_Texture *p_text = render_text(rend, font, pmask);
        if (u_text) { int tw,th; SDL_QueryTexture(u_text,NULL,NULL,&tw,&th); SDL_Rect d={ufield.x+4,ufield.y+1,tw,th}; SDL_RenderCopy(rend,u_text,NULL,&d); SDL_DestroyTexture(u_text); }
        if (p_text) { int tw,th; SDL_QueryTexture(p_text,NULL,NULL,&tw,&th); SDL_Rect d={pfield.x+4,pfield.y+1,tw,th}; SDL_RenderCopy(rend,p_text,NULL,&d); SDL_DestroyTexture(p_text); }

        /* buttons */
        const char *btn_continue = "< Continue >";
        const char *btn_create = "< Create >";
        SDL_Texture *b1 = render_text(rend,font,btn_continue);
        SDL_Texture *b2 = render_text(rend,font,btn_create);
        int bx = box_x + 24;
        int by = box_y + 120;
        if (b1) {
            int tw,th; SDL_QueryTexture(b1,NULL,NULL,&tw,&th);
            SDL_Rect d={bx,by,tw,th};
            if (focus==2) { SDL_SetRenderDrawColor(rend,60,60,60,255); SDL_Rect br={d.x-6,d.y-4,d.w+12,d.h+8}; SDL_RenderFillRect(rend,&br); }
            SDL_RenderCopy(rend,b1,NULL,&d); SDL_DestroyTexture(b1);
        }
        if (b2) {
            int tw,th; SDL_QueryTexture(b2,NULL,NULL,&tw,&th);
            SDL_Rect d={bx+160,by,tw,th};
            if (focus==3) { SDL_SetRenderDrawColor(rend,60,60,60,255); SDL_Rect br={d.x-6,d.y-4,d.w+12,d.h+8}; SDL_RenderFillRect(rend,&br); }
            SDL_RenderCopy(rend,b2,NULL,&d); SDL_DestroyTexture(b2);
        }

        /* message (red for errors) */
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

        /* present this frame (we assume caller won't call SDL_RenderPresent again immediately) */
        SDL_RenderPresent(rend);
    } /* end auto_render_modal */

    /* main modal event loop */
    while (1) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                SDL_StopTextInput();
                return NULL;
            } else if (ev.type == SDL_KEYDOWN) {
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
                        /* Continue -> attempt login */
                        if (strlen(username)==0 || strlen(password)==0) {
                            snprintf(message, sizeof(message), "ERROR: username and password required");
                        } else if (verify_credentials_secure(username, password)) {
                            /* create home and chdir */
                            if (create_home_dir_and_chdir(username, &posix_fs) != 0) {
                                snprintf(message, sizeof(message), "ERROR: failed to chdir to home/%s", username);
                            } else {
                                char *res = malloc(strlen(username)+1);
                                strcpy(res, username);
                                SDL_StopTextInput();
                                return res;
                            }
                        } else {
                            snprintf(message, sizeof(message), "ERROR: invalid username or password");
                            password[0]='\0'; pass_pos=0;
                        }
                    } else if (focus == 3) {
                        /* Create -> attempt to create account */
                        if (strlen(username)==0 || strlen(password)==0) {
                            snprintf(message, sizeof(message), "ERROR: username and password required");
                        } else {
                            int r = add_profile_secure(username, password);
                            if (r == 0) {
                                snprintf(message, sizeof(message), "Account created. Return to login and press Continue.");
                                password[0]='\0'; pass_pos=0; focus=0;
                            } else if (r == 1) {
                                snprintf(message, sizeof(message), "ERROR: username already taken");
                            } else {
                                snprintf(message, sizeof(message), "ERROR: failed to create account");
                            }
                        }
                    }
                } else if (k == SDLK_ESCAPE) {
                    SDL_StopTextInput();
                    return NULL;
                }
                /* after handling key, re-render */
                goto auto_render_modal;
            } else if (ev.type == SDL_TEXTINPUT) {
                const char *txt = ev.text.text;
                if (focus == 0) {
                    size_t cur = strlen(username);
                    size_t add = strlen(txt);
                    if (cur + add < sizeof(username)-1) {
                        strncat(username, txt, add);
                        uname_pos = strlen(username);
                    }
                } else if (focus == 1) {
                    size_t cur = strlen(password);
                    size_t add = strlen(txt);
                    if (cur + add < sizeof(password)-1) {
                        strncat(password, txt, add);
                        pass_pos = strlen(password);
                    }
                }
                goto auto_render_modal;
            }
        } /* end PollEvent */

        /* small delay so modal doesn't spin CPU */
        SDL_Delay(8);
    } /* end modal loop */

    /* unreachable */
    SDL_StopTextInput();
    return NULL;
}




/* small dynamic line buffer */
typedef struct { char **lines; int count; int cap; } LineBuf;
static void lb_init(LineBuf *lb) { lb->lines = NULL; lb->count = 0; lb->cap = 0; }

static void lb_clear(LineBuf *lb) {
    if (!lb) return;
    for (int i = 0; i < lb->count; ++i) {
        free(lb->lines[i]);
    }
    lb->count = 0;
    // keep allocated array around for reuse (avoid free/realloc churn)
}



static void lb_push(LineBuf *lb, const char *s) {
    if (!s) return;
    if (lb->count + 1 >= lb->cap) { lb->cap = lb->cap ? lb->cap * 2 : 256; lb->lines = realloc(lb->lines, lb->cap * sizeof(char*)); }
    lb->lines[lb->count++] = strdup(s);
    while (lb->count > 4000) { free(lb->lines[0]); memmove(&lb->lines[0], &lb->lines[1], (lb->count-1)*sizeof(char*)); lb->count--; }
}
static void lb_free(LineBuf *lb) { for (int i=0;i<lb->count;i++) free(lb->lines[i]); free(lb->lines); }

/* push a long text into lb with wrapping to visible columns */
static void lb_push_wrapped(LineBuf *lb, const char *s, int maxcols) {
    if (!s) return;
    size_t L = strlen(s);
    if (L == 0) { lb_push(lb, ""); return; }
    const char *p = s;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t chunklen = nl ? (size_t)(nl - p) : strlen(p);
        if ((int)chunklen <= maxcols) {
            char *tmp = malloc(chunklen + 1);
            if (!tmp) return;
            memcpy(tmp, p, chunklen); tmp[chunklen]=0;
            lb_push(lb, tmp);
            free(tmp);
        } else {
            size_t remaining = chunklen;
            const char *q = p;
            while (remaining > 0) {
                size_t take = remaining > (size_t)maxcols ? (size_t)maxcols : remaining;
                size_t used = take;
                if (take == (size_t)maxcols) {
                    size_t i;
                    for (i = take; i > 0; --i) {
                        if (q[i-1] == ' ' || q[i-1] == '\t') { used = i; break; }
                    }
                    if (used == 0) used = take;
                }
                char *tmp = malloc(used + 1);
                if (!tmp) return;
                memcpy(tmp, q, used); tmp[used]=0;
                lb_push(lb, tmp);
                free(tmp);
                q += used;
                while (*q == ' ' || *q == '\t') { q++; remaining--; }
                remaining = chunklen - (size_t)(q - p);
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
}

/* Create a soft circular corner shadow texture of size s x s.
   Black->transparent radial gradient, used at each corner.
*/
static SDL_Texture *create_corner_shadow(SDL_Renderer *rend, int s, int max_alpha)
{
    if (s <= 0) return NULL;
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, s, s, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return NULL;
    Uint32 *pix = (Uint32*)surf->pixels;
    float cx = (s-1)*0.0f; /* we'll use distance from corner (0,0) */
    float cy = (s-1)*0.0f;
    float maxd = sqrtf((float)(s*s) + (float)(s*s));
    for (int y = 0; y < s; ++y) {
        for (int x = 0; x < s; ++x) {
            float dx = (float)(s-1 - x); /* distance from bottom-right of this small quad when we flip */
            float dy = (float)(s-1 - y);
            /* distance from the corner (we'll flip copies so this formula OK) */
            float d = sqrtf((float)(x*x + y*y));
            float t = d / (float)s; /* 0..~1 */
            float alphaf = 0.0f;
            /* A quick falloff curve for a soft shadow */
            if (t < 0.95f) alphaf = (1.0f - powf(t, 2.4f)) * (float)max_alpha;
            else alphaf = 0.0f;
            if (alphaf < 0.0f) alphaf = 0.0f;
            if (alphaf > 255.0f) alphaf = 255.0f;
            Uint8 a = (Uint8)alphaf;
            Uint32 col = SDL_MapRGBA(surf->format, 0, 0, 0, a);
            pix[y * (surf->pitch/4) + x] = col;
        }
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(rend, surf);
    if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(surf);
    return tex;
}

/* Create a vertical side glow texture (w x h) that fades horizontally.
   If left==1 create left->right fade; if left==0 create right->left fade (same content may be flipped).
*/
static SDL_Texture *create_side_glow(SDL_Renderer *rend, int w, int h, int max_green, int left)
{
    if (w <= 0 || h <= 0) return NULL;
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return NULL;
    Uint32 *pix = (Uint32*)surf->pixels;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int px = left ? x : (w - 1 - x);
            float t = (float)px / (float)(w - 1); /* 0 at inner edge -> 1 at outer edge */
            /* We want a small bright band near inner edge, fall off quickly */
            float alphaf = (1.0f - powf(t, 1.8f)) * 0.55f; /* scale down so glow is faint */
            if (alphaf < 0.0f) alphaf = 0.0f;
            if (alphaf > 1.0f) alphaf = 1.0f;
            Uint8 a = (Uint8)(alphaf * 200.0f); /* max ~200 */
            /* grazing green tint (low red/blue) */
            Uint8 r = 8;
            Uint8 g = (Uint8) ( (float)max_green * (0.6f + 0.4f * (1.0f - t)) ); /* mild variation */
            Uint8 b = 8;
            Uint32 col = SDL_MapRGBA(surf->format, r, g, b, a);
            pix[y * (surf->pitch/4) + x] = col;
        }
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(rend, surf);
    if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_ADD); /* additive for glow */
    SDL_FreeSurface(surf);
    return tex;
}


/* spawn backend (same as yours) */
static pid_t spawn_backend(const char *path, int *wfd, int *rfd, int *efd) {
    int to_child[2], from_child[2], err_pipe[2];
    if (pipe(to_child) < 0) return -1;
    if (pipe(from_child) < 0) { close(to_child[0]); close(to_child[1]); return -1; }
    if (pipe(err_pipe) < 0) { close(to_child[0]); close(to_child[1]); close(from_child[0]); close(from_child[1]); return -1; }
    pid_t pid = fork();
    if (pid < 0) { close(to_child[0]); close(to_child[1]); close(from_child[0]); close(from_child[1]); close(err_pipe[0]); close(err_pipe[1]); return -1; }
    if (pid == 0) {
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        execl(path, path, (char*)NULL);
        _exit(127);
    }
    close(to_child[0]); close(from_child[1]); close(err_pipe[1]);
    *wfd = to_child[1]; *rfd = from_child[0]; *efd = err_pipe[0];
    fcntl(*rfd, F_SETFL, O_NONBLOCK);
    fcntl(*efd, F_SETFL, O_NONBLOCK);
    return pid;
}
static int backend_write_line(int wfd, const char *line) {
    size_t L = strlen(line);
    if (write(wfd, line, L) != (ssize_t)L) return -1;
    if (write(wfd, "\n", 1) != 1) return -1;
    return 0;
}

/* --- create radial vignette texture (RGBA, black fading toward edges) --- */
static SDL_Texture *create_vignette(SDL_Renderer *rend, int w, int h) {
    if (w <= 0 || h <= 0) return NULL;
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return NULL;
    Uint32 *pixels = (Uint32*)surf->pixels;
    float cx = (w-1) * 0.5f, cy = (h-1) * 0.5f;
    float maxd = sqrtf(cx*cx + cy*cy);
    for (int y=0;y<h;++y) {
        for (int x=0;x<w;++x) {
            float dx = (x - cx);
            float dy = (y - cy);
            float d = sqrtf(dx*dx + dy*dy) / maxd; // 0..1
            float a = 0.0f;
            if (d > 0.45f) {
                float t = (d - 0.45f) / (1.0f - 0.45f);
                a = powf(t, 1.6f) * 220.0f; // 0..220
            } else {
                a = 0.0f;
            }
            if (a > 255.0f) a = 255.0f;
            Uint8 aa = (Uint8)a;
            Uint32 col = SDL_MapRGBA(surf->format, 0, 0, 0, aa);
            pixels[y * (surf->pitch/4) + x] = col;
        }
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(rend, surf);
    if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(surf);
    return tex;
}

/* Draw a simple bevelled frame (outer border) */
static void draw_frame(SDL_Renderer *r, int x, int y, int w, int h) {
    // outer rim
    SDL_Rect rect = { x-10, y-10, w+20, h+20 };
    SDL_SetRenderDrawColor(r, 40, 32, 28, 255);
    SDL_RenderFillRect(r, &rect);
    // inner bevel layers
    SDL_Rect r1 = { x-6, y-6, w+12, h+12 };
    SDL_SetRenderDrawColor(r, 70, 58, 48, 255);
    SDL_RenderFillRect(r, &r1);
    SDL_Rect r2 = { x-3, y-3, w+6, h+6 };
    SDL_SetRenderDrawColor(r, 30, 20, 18, 255);
    SDL_RenderFillRect(r, &r2);
    // decorative inner rim
    SDL_Rect rim = { x-1, y-1, w+2, h+2 };
    SDL_SetRenderDrawColor(r, 10, 6, 4, 255);
    SDL_RenderDrawRect(r, &rim);
}

static void draw_filled_circle(SDL_Renderer *rend, int cx, int cy, int radius,
                               Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    if (radius <= 0) return;
    SDL_SetRenderDrawBlendMode(rend, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(rend, r, g, b, a);
    for (int dy = -radius; dy <= radius; ++dy) {
        int dx = (int)floor(sqrt((double)radius*radius - (double)dy*dy));
        SDL_RenderDrawLine(rend, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

/*----------------LED Helpers--------------*/

static void draw_led(SDL_Renderer *rend, int cx, int cy, int r,
                     Uint8 rr, Uint8 gg, Uint8 bb, int on)
{
    /* bezel / outer rim */
    draw_filled_circle(rend, cx, cy, r + 3,  36, 36, 36, 255);  /* bezel */
    draw_filled_circle(rend, cx, cy, r + 1,  12, 12, 12, 255);  /* inner rim */

    if (on) {
        /* soft glow */
        draw_filled_circle(rend, cx, cy, r * 2, rr, gg, bb, 60);
        /* bright core */
        draw_filled_circle(rend, cx, cy, r, rr, gg, bb, 220);
        /* small white highlight (upper-left) */
        draw_filled_circle(rend, cx - r/3, cy - r/3, r/3, 255, 255, 255, 200);
    } else {
        /* dark (off) core and a subtle specular */
        draw_filled_circle(rend, cx, cy, r, 24, 24, 24, 255);
        draw_filled_circle(rend, cx - r/3, cy - r/3, r/3, 80, 80, 80, 160);
    }
}
/* ----------------- end LED helpers ----------------- */

/* draw scanlines across given rect */
static void draw_scanlines(SDL_Renderer *r, int x, int y, int w, int h, int gap, int alpha) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    for (int yy = y; yy < y + h; yy += gap) {
        SDL_SetRenderDrawColor(r, 0, 0, 0, alpha); // subtle dark lines
        SDL_RenderDrawLine(r, x, yy, x + w, yy);
    }
}

/* draw a soft inner glow behind text: we approximate by drawing the same texture several times offset with low alpha */
static void render_text_with_glow(SDL_Renderer *rend, TTF_Font *font, const char *text, int px, int py) {
    if (!text || !*text) return;
    SDL_Color glowCol = {70, 255, 70, 80}; // green glow
    SDL_Color fg = {51, 255, 51, 255};

    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, fg);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(rend, surf);
    SDL_Texture *tex_glow = SDL_CreateTextureFromSurface(rend, surf); // reuse for glow passes
    SDL_FreeSurface(surf);
    if (!tex) { if (tex_glow) SDL_DestroyTexture(tex_glow); return; }
    SDL_SetTextureBlendMode(tex_glow, SDL_BLENDMODE_ADD);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    int tw, th;
    SDL_QueryTexture(tex, NULL, NULL, &tw, &th);

    // glow passes: offsets with low alpha (additive)
    const int pass_offsets[6][2] = {{-2,0},{2,0},{0,-2},{0,2},{-1,-1},{1,1}};
    for (int i=0;i<6;++i) {
        SDL_Rect dst = { px + pass_offsets[i][0], py + pass_offsets[i][1], tw, th };
        SDL_SetTextureColorMod(tex_glow, glowCol.r, glowCol.g, glowCol.b);
        SDL_SetTextureAlphaMod(tex_glow, 60);
        SDL_RenderCopy(rend, tex_glow, NULL, &dst);
    }

    // final crisp text
    SDL_Rect final = { px, py, tw, th };
    SDL_RenderCopy(rend, tex, NULL, &final);

    SDL_DestroyTexture(tex_glow);
    SDL_DestroyTexture(tex);
}

static void render_worn_sticker(SDL_Renderer *rend, TTF_Font *font, const char *text, int px, int py)
{
    if (!text || !*text || !font) return;

    /* text color (dark/worn brown for contrast) */
    SDL_Color textCol = { 48, 30, 18, 255 };

    SDL_Surface *s = TTF_RenderUTF8_Blended(font, text, textCol);
    if (!s) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(rend, s);
    int tw = s->w, th = s->h;
    SDL_FreeSurface(s);
    if (!tex) return;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    /* DULL ORANGE BACKGROUND for sticker */
    SDL_Rect bg = { px - 8, py - 4, tw + 16, th + 8 };

    SDL_SetRenderDrawBlendMode(rend, SDL_BLENDMODE_BLEND);
    /* base backing: dull orange */
    SDL_SetRenderDrawColor(rend, 160, 100, 50, 200);  /* dull orange, semi-transparent */
    SDL_RenderFillRect(rend, &bg);
    /* worn rim (muted brown/orange) */
    SDL_SetRenderDrawColor(rend, 120, 75, 45, 90);
    SDL_RenderDrawRect(rend, &bg);

    /* dark under-pass (gives partial missing-paint look) */
    SDL_SetTextureColorMod(tex, 40, 40, 40);
    SDL_SetTextureAlphaMod(tex, 90);
    SDL_Rect d1 = { px - 1, py + 1, tw, th };
    SDL_RenderCopy(rend, tex, NULL, &d1);

    /* main faded text (desaturated to match worn look) */
    SDL_SetTextureColorMod(tex, 200, 170, 130);
    SDL_SetTextureAlphaMod(tex, 210);
    SDL_Rect dst = { px, py, tw, th };
    SDL_RenderCopy(rend, tex, NULL, &dst);

    /* faint highlight pass */
    SDL_SetTextureColorMod(tex, 230, 220, 200);
    SDL_SetTextureAlphaMod(tex, 36);
    SDL_Rect d2 = { px + 1, py - 1, tw, th };
    SDL_RenderCopy(rend, tex, NULL, &d2);

    /* small deterministic scrape lines across sticker to add wear */
    SDL_SetRenderDrawBlendMode(rend, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < 5; ++i) {
        int sy = bg.y + 4 + (i * (bg.h - 8) / 5);
        int sx0 = bg.x + 4 + ((i * 7) & 0x1F);
        int sx1 = bg.x + bg.w - 6 - ((i * 13) & 0x1F);
        if (sx1 <= sx0) continue;
        SDL_SetRenderDrawColor(rend, 0, 0, 0, 40); /* subtle darker scrape */
        SDL_RenderDrawLine(rend, sx0, sy, sx1, sy);
        if ((i & 1) == 0) {
            SDL_SetRenderDrawColor(rend, 255, 255, 255, 14);
            SDL_RenderDrawLine(rend, sx0 + 2, sy - 1, sx1 - 2, sy - 1);
        }
    }

    SDL_DestroyTexture(tex);
}

/* safer atomic save: create temp file in same dir using strrchr instead of dirname/basename */
static int atomic_save_lines(const char *abs_path, LineBuf *buf)
{
    if (!abs_path || !buf) return -1;

    /* find directory part */
    const char *slash = strrchr(abs_path, '/');
    char dir[PATH_MAX];
    char tmp_template[PATH_MAX];

    if (slash == NULL) {
        /* no slash -> use current directory */
        if (snprintf(dir, sizeof(dir), ".") >= (int)sizeof(dir)) return -1;
    } else {
        size_t dlen = (size_t)(slash - abs_path);
        if (dlen >= sizeof(dir)) return -1;
        memcpy(dir, abs_path, dlen);
        dir[dlen] = '\0';
    }

    /* build temp template "<dir>/.<basename>.tmpXXXXXX" */
    const char *basename = (slash ? slash + 1 : abs_path);
    if (snprintf(tmp_template, sizeof(tmp_template), "%s/.%s.tmpXXXXXX", dir, basename) >= (int)sizeof(tmp_template))
        return -1;

    /* mkstemp requires a mutable buffer */
    int fd = mkstemp(tmp_template);
    if (fd < 0) {
        /* fallback: try direct write to target (best-effort) */
        FILE *f = fopen(abs_path, "wb");
        if (!f) return -1;
        for (int i = 0; i < buf->count; ++i) {
            if (fwrite(buf->lines[i], 1, strlen(buf->lines[i]), f) != strlen(buf->lines[i])) { fclose(f); return -1; }
            if (i < buf->count - 1) fputc('\n', f);
        }
        fclose(f);
        return 0;
    }

    FILE *tf = fdopen(fd, "wb");
    if (!tf) { close(fd); unlink(tmp_template); return -1; }

    /* write lines */
    for (int i = 0; i < buf->count; ++i) {
        size_t w = fwrite(buf->lines[i], 1, strlen(buf->lines[i]), tf);
        if (w != strlen(buf->lines[i])) { fclose(tf); unlink(tmp_template); return -1; }
        if (i < buf->count - 1) fputc('\n', tf);
    }
    fflush(tf);

    /* ensure data hits disk */
    if (fsync(fd) != 0) {
        /* ignore fsync failure, but attempt rename anyway */
    }

    fclose(tf); /* closes fd */

    /* now atomically replace target */
    if (rename(tmp_template, abs_path) != 0) {
        unlink(tmp_template);
        return -1;
    }

    return 0;
}


/* ---------------- CRT login helpers ---------------- */
static void push_login(LineBuf *lb, const char *username, const char *password,
                       int focus, const char *message)
{
    lb_clear(lb);
    lb_push(lb, "");
    lb_push(lb, "================== Cortez Terminal Login ==================");
    lb_push(lb, "");

    char buf[512];

    /* Username */
    snprintf(buf, sizeof(buf), " Username: %s%s", username ? username : "", focus==0 ? "_" : "");
    lb_push(lb, buf);

    /* Password (masked) */
    char pmask[256];
    size_t plen = password ? strlen(password) : 0;
    if (plen > sizeof(pmask)-1) plen = sizeof(pmask)-1;
    for (size_t i=0;i<plen;i++) pmask[i]='*';
    pmask[plen]='\0';
    snprintf(buf, sizeof(buf), " Password: %s%s", pmask, focus==1 ? "_" : "");
    lb_push(lb, buf);

    /* Buttons */
    char b1[32], b2[32];
    snprintf(b1, sizeof(b1), (focus==2) ? "> < Continue >" : "< Continue >");
    snprintf(b2, sizeof(b2), (focus==3) ? "> < Create >"   : "< Create >");
    snprintf(buf, sizeof(buf), "   %s     %s", b1, b2);
    lb_push(lb, "");
    lb_push(lb, buf);
    lb_push(lb, "");

    if (message && message[0]) {
        snprintf(buf, sizeof(buf), " %s", message);
        lb_push(lb, buf);
    } else {
        lb_push(lb, " Use arrows or TAB to move. Enter to activate. Esc to cancel.");
    }
    lb_push(lb, "");
}

/* blocking CRT login; returns malloc'd username or NULL */
static char *run_crt_login(SDL_Window *win, SDL_Renderer *rend, TTF_Font *font,
                           LineBuf *lb, int win_w, int win_h)
{
    char username[MAX_USERNAME]; username[0]='\0';
    char password[256]; password[0]='\0';
    int focus = 0;
    int uname_pos = 0, pass_pos = 0;
    char message[MAX_MSG]; message[0]='\0';
    SDL_Event ev;

    if (ensure_profiles_file_exists() != 0) {
        snprintf(message, sizeof(message), "ERROR: failed to create data/profiles.json");
    }

    push_login(lb, username, password, focus, message);

    SDL_StartTextInput();
    while (1) {
        /* request main loop redraw */
        SDL_Event ue; SDL_memset(&ue,0,sizeof(ue)); ue.type = SDL_USEREVENT; SDL_PushEvent(&ue);

        if (!SDL_WaitEvent(&ev)) { continue; }
        if (ev.type == SDL_QUIT) { SDL_StopTextInput(); return NULL; }
        if (ev.type == SDL_USEREVENT) { /* let main loop redraw then continue */ continue; }

        if (ev.type == SDL_KEYDOWN) {
            SDL_Keycode k = ev.key.keysym.sym;
            if (k == SDLK_TAB) focus = (focus+1)%4;
            else if (k == SDLK_UP) { if (focus>0) focus--; }
            else if (k == SDLK_DOWN) { if (focus<3) focus++; }
            else if (k == SDLK_LEFT) { if (focus>=2) focus=2; }
            else if (k == SDLK_RIGHT) { if (focus>=2) focus=3; }
            else if (k == SDLK_BACKSPACE) {
                if (focus==0 && uname_pos>0) { uname_pos--; username[uname_pos]='\0'; }
                else if (focus==1 && pass_pos>0) { pass_pos--; password[pass_pos]='\0'; }
            } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                if (focus==2) {
                    if (strlen(username)==0 || strlen(password)==0) {
                        snprintf(message, sizeof(message), "ERROR: username and password required");
                    } else if (verify_credentials_secure(username, password)) {
                        /* create home and chdir using frontend's posix_fs adapter */
                        if (create_home_dir_and_chdir(username, &posix_fs) != 0) {
                            snprintf(message, sizeof(message), "ERROR: failed to chdir to home/%s", username);
                        } else {
                            char *res = malloc(strlen(username)+1);
                            strcpy(res, username);
                            SDL_StopTextInput();
                            return res;
                        }
                    } else {
                        snprintf(message, sizeof(message), "ERROR: invalid username or password");
                        password[0]='\0'; pass_pos=0;
                    }
                } else if (focus==3) {
                    if (strlen(username)==0 || strlen(password)==0) {
                        snprintf(message, sizeof(message), "ERROR: username and password required");
                    } else {
                        int r = add_profile_secure(username, password);
                        if (r==0) {
                            snprintf(message, sizeof(message), "Account created. Return to login and press Continue.");
                            password[0]='\0'; pass_pos=0; focus=0;
                        } else if (r==1) {
                            snprintf(message, sizeof(message), "ERROR: username already taken");
                        } else {
                            snprintf(message, sizeof(message), "ERROR: failed to create account");
                        }
                    }
                }
            } else if (k==SDLK_ESCAPE) {
                SDL_StopTextInput();
                return NULL;
            }
            push_login(lb, username, password, focus, message);
        } else if (ev.type == SDL_TEXTINPUT) {
            const char *txt = ev.text.text;
            if (focus==0) {
                size_t cur = strlen(username), add = strlen(txt);
                if (cur + add < sizeof(username)-1) {
                    strncat(username, txt, add);
                    uname_pos = strlen(username);
                }
            } else if (focus==1) {
                size_t cur = strlen(password), add = strlen(txt);
                if (cur + add < sizeof(password)-1) {
                    strncat(password, txt, add);
                    pass_pos = strlen(password);
                }
            }
            push_login(lb, username, password, focus, message);
        }
    }
    /* unreachable */
    SDL_StopTextInput();
    return NULL;
}


/* ---------- End CRT-native login flow ---------- */



/* ---------- improved frontend editor (atomic save + save prompt) ----------
   Ctrl+S -> show prompt "Save changes to <file>? (Y/n)".
   Y or Enter saves (atomic), N cancels.
   Esc exits editor (no prompt). */
static void run_simple_editor(const char *filename, SDL_Renderer *rend, TTF_Font *font,
                              int screen_x, int screen_y, int screen_w, int screen_h, const char *backend_cwd,
                              int backend_running)
{

        /* --- CRT composition resources for the editor --- */
    SDL_Texture *screen_tex = SDL_CreateTexture(rend, SDL_PIXELFORMAT_RGBA8888,
                                                SDL_TEXTUREACCESS_TARGET, screen_w - 16, screen_h - 16);
    if (screen_tex) SDL_SetTextureBlendMode(screen_tex, SDL_BLENDMODE_BLEND);
    SDL_Texture *vignette = create_vignette(rend, screen_w - 16, screen_h - 16);

    if (!filename || !*filename) return;

    /* resolve filename to absolute path (so saving goes where frontend expects) */
char abs_path[PATH_MAX];
if (filename[0] == '/') {
    strncpy(abs_path, filename, PATH_MAX-1);
    abs_path[PATH_MAX-1] = 0;
} else {
    snprintf(abs_path, sizeof(abs_path), "%s/%s", backend_cwd, filename);
}
    LineBuf ed; lb_init(&ed);

    /* load file lines into ed */
    FILE *f = fopen(abs_path, "rb");
    if (f) {
        char *line = NULL;
        size_t cap = 0;
        ssize_t n;
        while ((n = getline(&line, &cap, f)) >= 0) {
            while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) { line[n-1] = 0; --n; }
            lb_push(&ed, line);
        }
        free(line);
        fclose(f);
    } else {
        lb_push(&ed, "");
    }

    int cur_r = 0, cur_c = 0;
    if (ed.count == 0) lb_push(&ed, "");
    if (cur_r >= ed.count) cur_r = ed.count - 1;
    if (cur_c > (int)strlen(ed.lines[cur_r])) cur_c = strlen(ed.lines[cur_r]);

    SDL_StartTextInput();

    int editing = 1;
    int show_save_prompt = 0; /* 0=none, 1=prompt visible */
    int line_h = FONT_SIZE + LINE_SPACING;
    int inset_x = 18, inset_y = 18;
    int avail_h = (screen_h - 16) - (inset_y * 2) - INPUT_HEIGHT - 16;
    int visible_rows = avail_h / line_h;
    if (visible_rows < 3) visible_rows = 3;
    int scroll_top = 0;

    /* transient status message after save */
    char status_msg[256] = "";
    Uint32 status_until = 0; // SDL_GetTicks() timestamp until status visible
    

    while (editing) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { editing = 0; break; }
            else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                SDL_GetWindowSize(SDL_GetWindowFromID(ev.window.windowID), &screen_w, &screen_h);
                avail_h = (screen_h - 16) - (inset_y * 2) - INPUT_HEIGHT - 16;
                visible_rows = avail_h / line_h; if (visible_rows < 3) visible_rows = 3;
                if (screen_tex) { SDL_DestroyTexture(screen_tex); screen_tex = NULL; }
                if (screen_w - 16 > 0 && screen_h - 16 > 0) {
                    screen_tex = SDL_CreateTexture(rend, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, screen_w - 16, screen_h - 16);
                    if (screen_tex) SDL_SetTextureBlendMode(screen_tex, SDL_BLENDMODE_BLEND);
                }
                if (vignette) { SDL_DestroyTexture(vignette); vignette = NULL; }
                vignette = create_vignette(rend, screen_w - 16, screen_h - 16);

            } else if (ev.type == SDL_TEXTINPUT && !show_save_prompt) {
                const char *txt = ev.text.text;
                char *ln = ed.lines[cur_r];
                size_t len = strlen(ln);
                size_t add = strlen(txt);
                char *newln = malloc(len + add + 1);
                if (!newln) continue;
                memcpy(newln, ln, cur_c);
                memcpy(newln + cur_c, txt, add);
                memcpy(newln + cur_c + add, ln + cur_c, len - cur_c + 1);
                free(ed.lines[cur_r]);
                ed.lines[cur_r] = newln;
                cur_c += add;
            } else if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode k = ev.key.keysym.sym;
                int mods = ev.key.keysym.mod;

                if (show_save_prompt) {
                    /* handle prompt choices: Y/Enter => save, N => cancel */
                    if (k == SDLK_y || k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                        int res = atomic_save_lines(abs_path, &ed);
                        if (res == 0) {
                            snprintf(status_msg, sizeof(status_msg), "Wrote %d lines to %s", ed.count, abs_path);
                        } else {
                            snprintf(status_msg, sizeof(status_msg), "Save failed: %s", strerror(errno));
                        }
                        status_until = SDL_GetTicks() + 1500; /* show for 1.5s */
                        show_save_prompt = 0;
                    } else if (k == SDLK_n || k == SDLK_ESCAPE) {
                        /* cancel prompt */
                        show_save_prompt = 0;
                    }
                } else {
                    /* normal editor key handling */
                    if ((mods & KMOD_CTRL) && k == SDLK_s) { /* Ctrl-S: show save prompt */
                        show_save_prompt = 1;
                    } else if (k == SDLK_ESCAPE) {
                        editing = 0; break;
                    } else if (k == SDLK_BACKSPACE) {
                        if (cur_c > 0) {
                            char *ln = ed.lines[cur_r];
                            size_t len = strlen(ln);
                            memmove(ln + cur_c - 1, ln + cur_c, len - cur_c + 1);
                            cur_c--;
                        } else if (cur_r > 0) {
                            int prev = cur_r - 1;
                            size_t l0 = strlen(ed.lines[prev]);
                            size_t l1 = strlen(ed.lines[cur_r]);
                            char *newln = malloc(l0 + l1 + 1);
                            if (newln) {
                                memcpy(newln, ed.lines[prev], l0);
                                memcpy(newln + l0, ed.lines[cur_r], l1 + 1);
                                free(ed.lines[prev]);
                                ed.lines[prev] = newln;
                            }
                            free(ed.lines[cur_r]);
                            memmove(&ed.lines[cur_r], &ed.lines[cur_r+1], (ed.count - cur_r - 1) * sizeof(char*));
                            ed.count--;
                            cur_r = prev;
                            cur_c = l0;
                        }
                    } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                        char *ln = ed.lines[cur_r];
                        size_t len = strlen(ln);
                        char *left = malloc(cur_c + 1);
                        char *right = malloc(len - cur_c + 1);
                        if (left && right) {
                            memcpy(left, ln, cur_c); left[cur_c] = 0;
                            memcpy(right, ln + cur_c, len - cur_c + 1);
                            free(ed.lines[cur_r]);
                            ed.lines[cur_r] = left;
                            if (ed.count + 1 >= ed.cap) { ed.cap = ed.cap ? ed.cap * 2 : 16; ed.lines = realloc(ed.lines, ed.cap * sizeof(char*)); }
                            memmove(&ed.lines[cur_r+2], &ed.lines[cur_r+1], (ed.count - cur_r - 1) * sizeof(char*));
                            ed.lines[cur_r+1] = right;
                            ed.count++;
                            cur_r++;
                            cur_c = 0;
                        } else { free(left); free(right); }
                    } else if (k == SDLK_UP) {
                        if (cur_r > 0) { cur_r--; if (cur_c > (int)strlen(ed.lines[cur_r])) cur_c = strlen(ed.lines[cur_r]); }
                        if (cur_r < scroll_top) scroll_top = cur_r;
                    } else if (k == SDLK_DOWN) {
                        if (cur_r + 1 < ed.count) { cur_r++; if (cur_c > (int)strlen(ed.lines[cur_r])) cur_c = strlen(ed.lines[cur_r]); }
                        if (cur_r >= scroll_top + visible_rows) scroll_top = cur_r - visible_rows + 1;
                    } else if (k == SDLK_LEFT) {
                        if (cur_c > 0) cur_c--;
                        else if (cur_r > 0) { cur_r--; cur_c = strlen(ed.lines[cur_r]); if (cur_r < scroll_top) scroll_top = cur_r; }
                    } else if (k == SDLK_RIGHT) {
                        if (cur_c < (int)strlen(ed.lines[cur_r])) cur_c++;
                        else if (cur_r + 1 < ed.count) { cur_r++; cur_c = 0; if (cur_r >= scroll_top + visible_rows) scroll_top = cur_r - visible_rows + 1; }
                    }
                }
            }
        }

         /* ---------------- rendering (editor -> screen_tex -> composite) ---------------- */
        /* render editor content into the offscreen CRT texture first */
        if (screen_tex) SDL_SetRenderTarget(rend, screen_tex);

        // inner CRT background (draw into render target)
        SDL_SetRenderDrawColor(rend, 6, 18, 6, 255);
        SDL_RenderClear(rend);

        // draw buffered editor lines with regular TTF (no glow to keep editor text readable)
        int e_line_h = FONT_SIZE + LINE_SPACING;
        int e_inset_x = 18;
        int e_inset_y = 18;
        int e_avail_h = (screen_h - 16) - (e_inset_y * 2) - INPUT_HEIGHT - 16;
        int e_max_visible = e_avail_h / e_line_h;
        if (e_max_visible < 1) e_max_visible = 1;
        int e_start = scroll_top;
        int e_end = e_start + visible_rows; if (e_end > ed.count) e_end = ed.count;
        int ey = e_inset_y;
for (int r = e_start; r < e_end; ++r) {
    render_text_with_glow(rend, font, ed.lines[r], e_inset_x, ey);
    ey += e_line_h;
}


        // cursor inside render target
        if (cur_r >= e_start && cur_r < e_end) {
            int cursor_x = e_inset_x + (int)(cur_c * (FONT_SIZE * 0.6f));
            int cursor_y = e_inset_y + (cur_r - e_start) * e_line_h;
            SDL_SetRenderDrawBlendMode(rend, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(rend, 180, 220, 180, 120);
            SDL_Rect cur = { cursor_x, cursor_y, (int)(FONT_SIZE * 0.6f), e_line_h };
            SDL_RenderFillRect(rend, &cur);
        }

        // status bar (render into target)
        char sb[256];
        snprintf(sb, sizeof(sb), "cedit: %s  (Ctrl-S save, Esc exit)  Ln %d Col %d", abs_path, cur_r+1, cur_c);
        SDL_Surface *ss = TTF_RenderUTF8_Blended(font, sb, (SDL_Color){200,180,140,255});
        render_text_with_glow(rend, font, sb, e_inset_x, (screen_h - 16) - e_inset_y - INPUT_HEIGHT - 6);


        // save prompt overlay rendering into render target (if visible)
        if (show_save_prompt) {
            char q[512];
            snprintf(q, sizeof(q), "Save changes to %s ? (Y/n)", abs_path);
            SDL_Surface *qs = TTF_RenderUTF8_Blended(font, q, (SDL_Color){255,255,255,255});
            if (qs) {
                SDL_Texture *qt = SDL_CreateTextureFromSurface(rend, qs);
                int qw = qs->w + 16;
                int qh = qs->h + 12;
                SDL_Rect box = { e_inset_x + ( (screen_w - 16) - qw)/2, ( (screen_h - 16) - qh)/2, qw, qh };
                SDL_SetRenderDrawColor(rend, 20, 40, 20, 220);
                SDL_RenderFillRect(rend, &box);
                SDL_SetRenderDrawColor(rend, 100, 100, 100, 180);
                SDL_RenderDrawRect(rend, &box);
                SDL_Rect qd = { box.x + 8, box.y + 6, qs->w, qs->h };
                SDL_RenderCopy(rend, qt, NULL, &qd);
                SDL_DestroyTexture(qt);
                SDL_FreeSurface(qs);
            }
        }

        // transient status (in render target)
        if (status_until && SDL_GetTicks() < status_until) {
            SDL_Surface *ms = TTF_RenderUTF8_Blended(font, status_msg, (SDL_Color){200,200,200,255});
            if (ms) {
                SDL_Texture *mt = SDL_CreateTextureFromSurface(rend, ms);
                SDL_Rect md = { e_inset_x, (screen_h - 16) - e_inset_x - INPUT_HEIGHT - 6 - 24, ms->w, ms->h };
                SDL_RenderCopy(rend, mt, NULL, &md);
                SDL_DestroyTexture(mt);
                SDL_FreeSurface(ms);
            }
        }

        // finished drawing editor contents into render target
        SDL_SetRenderTarget(rend, NULL);

        /* composite: apply flicker / color mod / vignette / scanlines and then draw LEDs & sticker */
        double now = SDL_GetTicks() / 1000.0;
        float base = 0.90f + 0.08f * sinf((float)(now * 6.0));
        if ((rand() % 1000) < 6) base -= ((rand() % 40) / 255.0f);
        if ((rand() % 1000) < 4) base += ((rand() % 30) / 255.0f);
        if (base < 0.5f) base = 0.5f;
        if (base > 1.05f) base = 1.05f;
        Uint8 alpha = (Uint8)(base * 255.0f);
        SDL_SetTextureAlphaMod(screen_tex, alpha);
        int green_mod = 180 + (int)(75.0f * (base - 0.9f) * 4.0f);
        if (green_mod < 180) green_mod = 180;
        if (green_mod > 255) green_mod = 255;
        SDL_SetTextureColorMod(screen_tex, 180, green_mod, 180);

        // small jitter
        int jitter_x = 0, jitter_y = 0;
        if ((rand() % 100) < 8) { jitter_x = (rand() % 3) - 1; jitter_y = (rand() % 3) - 1; }

        SDL_Rect dst = { screen_x + 8 + jitter_x, screen_y + 8 + jitter_y, screen_w - 16, screen_h - 16 };
        // background + frame
        SDL_SetRenderDrawColor(rend, 24, 20, 18, 255);
        SDL_RenderClear(rend);
        draw_frame(rend, screen_x, screen_y, screen_w, screen_h);

        // blit the CRT content
        SDL_RenderCopy(rend, screen_tex, NULL, &dst);

        // vignette overlay
        if (vignette) SDL_RenderCopy(rend, vignette, NULL, &dst);

        // scanlines
        int scan_alpha = 16 + (int)((1.0f - base) * 80.0f);
        if (scan_alpha < 8) scan_alpha = 8;
        draw_scanlines(rend, dst.x, dst.y, dst.w, dst.h, 3, scan_alpha);


/* LED positions*/
int led_r = 8;
int led_spacing = 36;
int center_x = screen_x + (screen_w / 2) - (led_spacing / 2);
int leds_y = screen_y + screen_h + 24;

/* clamp against actual renderer/window output height (not CRT height) */
int out_w = 0, out_h = 0;
SDL_GetRendererOutputSize(rend, &out_w, &out_h);
if (leds_y + led_r + 4 > out_h) leds_y = out_h - led_r - 8;

/* draw LEDs: green on when backend_running, red on when not */
draw_led(rend, center_x,               leds_y, led_r,  0, 220, 0, backend_running ? 1 : 0);
draw_led(rend, center_x + led_spacing, leds_y, led_r, 220,   0, 0, backend_running ? 0 : 1);

/* sticker anchored relative to LEDs exactly like main */
int sticker_x = screen_x + screen_w - 180;
int sticker_y = leds_y - 6;
render_worn_sticker(rend, font, sticker_text, sticker_x, sticker_y);


        SDL_RenderPresent(rend);
        SDL_Delay(16);

}
}

static int g_mixer_device_open = 0;

static void play_audio_in_frontend(const char *path, LineBuf *lb) {
    
    if (!g_mixer_device_open) {
        if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0) {
            char err_msg[512];
            snprintf(err_msg, sizeof(err_msg), "[audio-err] Could not open audio device: %s", Mix_GetError());
            lb_push(lb, err_msg);
            return; // Fail gracefully
        }
        g_mixer_device_open = 1; // Mark it as open for future calls
    }

    if (g_current_music != NULL) {
        Mix_HaltMusic();
        Mix_FreeMusic(g_current_music);
        g_current_music = NULL;
    }

    g_current_music = Mix_LoadMUS(path);
    if (!g_current_music) {
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg), "[audio-err] Failed to load '%s': %s", path, Mix_GetError());
        lb_push(lb, err_msg);
        return;
    }

    if (Mix_PlayMusic(g_current_music, 1) == -1) { // Play once
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg), "[audio-err] Failed to play '%s': %s", path, Mix_GetError());
        lb_push(lb, err_msg);
        Mix_FreeMusic(g_current_music);
        g_current_music = NULL;
    } else {
        char msg[512];
        snprintf(msg, sizeof(msg), "[audio] Now playing: %s", path);
        lb_push(lb, msg);
    }
}

static void render_pixelated_surface_pass(SDL_Renderer *rend, SDL_Surface *surface_to_render, int render_w, int render_h)
{
    if (!surface_to_render) return;

    const SDL_Color green_palette[] = {
        {10, 45, 10, 255},
        {30, 110, 30, 255},
        {51, 205, 51, 255},
        {140, 255, 140, 255},
        {210, 255, 210, 255}
    };
    const int palette_size = sizeof(green_palette) / sizeof(green_palette[0]);

    const int PIXEL_COLS = 128;
    const int PIXEL_ROWS = 96;

    float pixel_block_w = (float)render_w / PIXEL_COLS;
    float pixel_block_h = (float)render_h / PIXEL_ROWS;

    Uint32 *pixels = (Uint32 *)surface_to_render->pixels;
    int src_w = surface_to_render->w;
    int src_h = surface_to_render->h;

    for (int y = 0; y < PIXEL_ROWS; ++y) {
        for (int x = 0; x < PIXEL_COLS; ++x) {
            int src_x_start = (int)((float)x / PIXEL_COLS * src_w);
            int src_y_start = (int)((float)y / PIXEL_ROWS * src_h);
            int src_x_end = (int)((float)(x + 1) / PIXEL_COLS * src_w);
            int src_y_end = (int)((float)(y + 1) / PIXEL_ROWS * src_h);

            double total_brightness = 0;
            int sample_count = 0;
            for (int sy = src_y_start; sy < src_y_end; ++sy) {
                for (int sx = src_x_start; sx < src_x_end; ++sx) {
                    Uint32 pixel_data = pixels[sy * src_w + sx];
                    Uint8 r, g, b, a;
                    SDL_GetRGBA(pixel_data, surface_to_render->format, &r, &g, &b, &a);
                    double brightness = (0.299 * r + 0.587 * g + 0.114 * b) / 255.0;
                    total_brightness += brightness;
                    sample_count++;
                }
            }
            
            if (sample_count == 0) continue;
            double avg_brightness = total_brightness / sample_count;

            int palette_index = (int)(avg_brightness * (palette_size));
            if (palette_index < 0) palette_index = 0;
            if (palette_index >= palette_size) palette_index = palette_size - 1;
            SDL_Color final_color = green_palette[palette_index];
            
            int x_pos = (int)(x * pixel_block_w + 0.5f);
            int y_pos = (int)(y * pixel_block_h + 0.5f);
            int next_x_pos = (int)((x + 1) * pixel_block_w + 0.5f);
            int next_y_pos = (int)((y + 1) * pixel_block_h + 0.5f);

            SDL_Rect pixel_rect = { x_pos, y_pos, next_x_pos - x_pos, next_y_pos - y_pos };
            SDL_SetRenderDrawColor(rend, final_color.r, final_color.g, final_color.b, 255);
            SDL_RenderFillRect(rend, &pixel_rect);
        }
    }
}



/* Save node content by sending n-write command to backend */
static int save_node_content(int wfd, int node_id, LineBuf *buf) {
    if (wfd < 0 || !buf) return -1;
    
    // Calculate total size
    int total_size = 0;
    for (int i = 0; i < buf->count; ++i) {
        total_size += strlen(buf->lines[i]) + 1; // +1 for newline
    }
    
    // Send command
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "n-write %d %d", node_id, total_size);
    backend_write_line(wfd, cmd);
    
    // Send data
    for (int i = 0; i < buf->count; ++i) {
        write(wfd, buf->lines[i], strlen(buf->lines[i]));
        write(wfd, "\n", 1);
    }
    return 0;
}

static void run_node_editor(int node_id, LineBuf *initial_content, SDL_Renderer *rend, TTF_Font *font,
                            int screen_x, int screen_y, int screen_w, int screen_h, int wfd, const char *sticker_text)
{
    SDL_Texture *screen_tex = SDL_CreateTexture(rend, SDL_PIXELFORMAT_RGBA8888,
                                                SDL_TEXTUREACCESS_TARGET, screen_w - 16, screen_h - 16);
    if (screen_tex) SDL_SetTextureBlendMode(screen_tex, SDL_BLENDMODE_BLEND);
    SDL_Texture *vignette = create_vignette(rend, screen_w - 16, screen_h - 16);

    LineBuf ed; lb_init(&ed);
    // Copy initial content
    if (initial_content) {
        for (int i = 0; i < initial_content->count; ++i) {
            lb_push(&ed, initial_content->lines[i]);
        }
    }
    if (ed.count == 0) lb_push(&ed, "");

    int cur_r = 0, cur_c = 0;
    
    SDL_StartTextInput();
    int editing = 1;
    int show_save_prompt = 0;
    int line_h = FONT_SIZE + LINE_SPACING;
    int inset_x = 18, inset_y = 18;
    int avail_h = (screen_h - 16) - (inset_y * 2) - INPUT_HEIGHT - 16;
    int visible_rows = avail_h / line_h;
    if (visible_rows < 3) visible_rows = 3;
    int scroll_top = 0;
    
    char status_msg[256] = "";
    Uint32 status_until = 0;

    while (editing) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { editing = 0; break; }
            else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                SDL_GetWindowSize(SDL_GetWindowFromID(ev.window.windowID), &screen_w, &screen_h);
                avail_h = (screen_h - 16) - (inset_y * 2) - INPUT_HEIGHT - 16;
                visible_rows = avail_h / line_h; if (visible_rows < 3) visible_rows = 3;
                if (screen_tex) { SDL_DestroyTexture(screen_tex); screen_tex = NULL; }
                if (screen_w - 16 > 0 && screen_h - 16 > 0) {
                    screen_tex = SDL_CreateTexture(rend, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, screen_w - 16, screen_h - 16);
                    if (screen_tex) SDL_SetTextureBlendMode(screen_tex, SDL_BLENDMODE_BLEND);
                }
                if (vignette) { SDL_DestroyTexture(vignette); vignette = NULL; }
                vignette = create_vignette(rend, screen_w - 16, screen_h - 16);
            } else if (ev.type == SDL_TEXTINPUT && !show_save_prompt) {
                const char *txt = ev.text.text;
                char *ln = ed.lines[cur_r];
                size_t len = strlen(ln);
                size_t add = strlen(txt);
                char *newln = malloc(len + add + 1);
                if (!newln) continue;
                memcpy(newln, ln, cur_c);
                memcpy(newln + cur_c, txt, add);
                memcpy(newln + cur_c + add, ln + cur_c, len - cur_c + 1);
                free(ed.lines[cur_r]);
                ed.lines[cur_r] = newln;
                cur_c += add;
            } else if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode k = ev.key.keysym.sym;
                int mods = ev.key.keysym.mod;

                if (show_save_prompt) {
                    if (k == SDLK_y || k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                        int res = save_node_content(wfd, node_id, &ed);
                        if (res == 0) {
                            snprintf(status_msg, sizeof(status_msg), "Saved to Node %d", node_id);
                        } else {
                            snprintf(status_msg, sizeof(status_msg), "Save failed");
                        }
                        status_until = SDL_GetTicks() + 1500;
                        show_save_prompt = 0;
                    } else if (k == SDLK_n || k == SDLK_ESCAPE) {
                        show_save_prompt = 0;
                    }
                } else {
                    if ((mods & KMOD_CTRL) && k == SDLK_s) {
                        show_save_prompt = 1;
                    } else if (k == SDLK_ESCAPE) {
                        editing = 0; break;
                    } else if (k == SDLK_BACKSPACE) {
                        if (cur_c > 0) {
                            char *ln = ed.lines[cur_r];
                            size_t len = strlen(ln);
                            memmove(ln + cur_c - 1, ln + cur_c, len - cur_c + 1);
                            cur_c--;
                        } else if (cur_r > 0) {
                            int prev = cur_r - 1;
                            size_t l0 = strlen(ed.lines[prev]);
                            size_t l1 = strlen(ed.lines[cur_r]);
                            char *newln = malloc(l0 + l1 + 1);
                            if (newln) {
                                memcpy(newln, ed.lines[prev], l0);
                                memcpy(newln + l0, ed.lines[cur_r], l1 + 1);
                                free(ed.lines[prev]);
                                ed.lines[prev] = newln;
                            }
                            free(ed.lines[cur_r]);
                            memmove(&ed.lines[cur_r], &ed.lines[cur_r+1], (ed.count - cur_r - 1) * sizeof(char*));
                            ed.count--;
                            cur_r = prev;
                            cur_c = l0;
                        }
                    } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                        char *ln = ed.lines[cur_r];
                        size_t len = strlen(ln);
                        char *left = malloc(cur_c + 1);
                        char *right = malloc(len - cur_c + 1);
                        if (left && right) {
                            memcpy(left, ln, cur_c); left[cur_c] = 0;
                            memcpy(right, ln + cur_c, len - cur_c + 1);
                            free(ed.lines[cur_r]);
                            ed.lines[cur_r] = left;
                            if (ed.count + 1 >= ed.cap) { ed.cap = ed.cap ? ed.cap * 2 : 16; ed.lines = realloc(ed.lines, ed.cap * sizeof(char*)); }
                            memmove(&ed.lines[cur_r+2], &ed.lines[cur_r+1], (ed.count - cur_r - 1) * sizeof(char*));
                            ed.lines[cur_r+1] = right;
                            ed.count++;
                            cur_r++;
                            cur_c = 0;
                        } else { free(left); free(right); }
                    } else if (k == SDLK_UP) {
                        if (cur_r > 0) { cur_r--; if (cur_c > (int)strlen(ed.lines[cur_r])) cur_c = strlen(ed.lines[cur_r]); }
                        if (cur_r < scroll_top) scroll_top = cur_r;
                    } else if (k == SDLK_DOWN) {
                        if (cur_r + 1 < ed.count) { cur_r++; if (cur_c > (int)strlen(ed.lines[cur_r])) cur_c = strlen(ed.lines[cur_r]); }
                        if (cur_r >= scroll_top + visible_rows) scroll_top = cur_r - visible_rows + 1;
                    } else if (k == SDLK_LEFT) {
                        if (cur_c > 0) cur_c--;
                        else if (cur_r > 0) { cur_r--; cur_c = strlen(ed.lines[cur_r]); if (cur_r < scroll_top) scroll_top = cur_r; }
                    } else if (k == SDLK_RIGHT) {
                        if (cur_c < (int)strlen(ed.lines[cur_r])) cur_c++;
                        else if (cur_r + 1 < ed.count) { cur_r++; cur_c = 0; if (cur_r >= scroll_top + visible_rows) scroll_top = cur_r - visible_rows + 1; }
                    }
                }
            }
        }

        // Rendering (Simplified copy of run_simple_editor rendering)
        if (screen_tex) SDL_SetRenderTarget(rend, screen_tex);
        SDL_SetRenderDrawColor(rend, 6, 18, 6, 255);
        SDL_RenderClear(rend);

        int e_line_h = FONT_SIZE + LINE_SPACING;
        int e_inset_x = 18;
        int e_inset_y = 18;
        int e_avail_h = (screen_h - 16) - (e_inset_y * 2) - INPUT_HEIGHT - 16;
        int e_max_visible = e_avail_h / e_line_h;
        if (e_max_visible < 1) e_max_visible = 1;
        int e_start = scroll_top;
        int e_end = e_start + visible_rows; if (e_end > ed.count) e_end = ed.count;
        int ey = e_inset_y;
        for (int r = e_start; r < e_end; ++r) {
            render_text_with_glow(rend, font, ed.lines[r], e_inset_x, ey);
            ey += e_line_h;
        }

        if (cur_r >= e_start && cur_r < e_end) {
            int cursor_x = e_inset_x + (int)(cur_c * (FONT_SIZE * 0.6f));
            int cursor_y = e_inset_y + (cur_r - e_start) * e_line_h;
            SDL_SetRenderDrawBlendMode(rend, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(rend, 180, 220, 180, 120);
            SDL_Rect cur = { cursor_x, cursor_y, (int)(FONT_SIZE * 0.6f), e_line_h };
            SDL_RenderFillRect(rend, &cur);
        }

        char sb[256];
        snprintf(sb, sizeof(sb), "Node %d  (Ctrl-S save, Esc exit)  Ln %d Col %d", node_id, cur_r+1, cur_c);
        SDL_Surface *ss = TTF_RenderUTF8_Blended(font, sb, (SDL_Color){200,180,140,255});
        render_text_with_glow(rend, font, sb, e_inset_x, (screen_h - 16) - e_inset_y - INPUT_HEIGHT - 6);

        if (show_save_prompt) {
            char q[512];
            snprintf(q, sizeof(q), "Save changes to Node %d? (Y/n)", node_id);
            SDL_Surface *qs = TTF_RenderUTF8_Blended(font, q, (SDL_Color){255,255,255,255});
            if (qs) {
                SDL_Texture *qt = SDL_CreateTextureFromSurface(rend, qs);
                int qw = qs->w + 16;
                int qh = qs->h + 12;
                SDL_Rect box = { e_inset_x + ( (screen_w - 16) - qw)/2, ( (screen_h - 16) - qh)/2, qw, qh };
                SDL_SetRenderDrawColor(rend, 20, 40, 20, 220);
                SDL_RenderFillRect(rend, &box);
                SDL_SetRenderDrawColor(rend, 100, 100, 100, 180);
                SDL_RenderDrawRect(rend, &box);
                SDL_Rect qd = { box.x + 8, box.y + 6, qs->w, qs->h };
                SDL_RenderCopy(rend, qt, NULL, &qd);
                SDL_DestroyTexture(qt);
                SDL_FreeSurface(qs);
            }
        }

        if (status_until && SDL_GetTicks() < status_until) {
            SDL_Surface *ms = TTF_RenderUTF8_Blended(font, status_msg, (SDL_Color){200,200,200,255});
            if (ms) {
                SDL_Texture *mt = SDL_CreateTextureFromSurface(rend, ms);
                SDL_Rect md = { e_inset_x, (screen_h - 16) - e_inset_x - INPUT_HEIGHT - 6 - 24, ms->w, ms->h };
                SDL_RenderCopy(rend, mt, NULL, &md);
                SDL_DestroyTexture(mt);
                SDL_FreeSurface(ms);
            }
        }

        SDL_SetRenderTarget(rend, NULL);

        // Composite
        double now = SDL_GetTicks() / 1000.0;
        float base = 0.90f + 0.08f * sinf((float)(now * 6.0));
        if ((rand() % 1000) < 6) base -= ((rand() % 40) / 255.0f);
        if ((rand() % 1000) < 4) base += ((rand() % 30) / 255.0f);
        if (base < 0.5f) base = 0.5f;
        if (base > 1.05f) base = 1.05f;
        Uint8 alpha = (Uint8)(base * 255.0f);
        SDL_SetTextureAlphaMod(screen_tex, alpha);
        int green_mod = 180 + (int)(75.0f * (base - 0.9f) * 4.0f);
        if (green_mod < 180) green_mod = 180;
        if (green_mod > 255) green_mod = 255;
        SDL_SetTextureColorMod(screen_tex, 180, green_mod, 180);

        // small jitter
        int jitter_x = 0, jitter_y = 0;
        if ((rand() % 100) < 8) { jitter_x = (rand() % 3) - 1; jitter_y = (rand() % 3) - 1; }

        SDL_Rect dst = { screen_x + 8 + jitter_x, screen_y + 8 + jitter_y, screen_w - 16, screen_h - 16 };
        
        SDL_SetRenderDrawColor(rend, 24, 20, 18, 255);
        SDL_RenderClear(rend);
        draw_frame(rend, screen_x, screen_y, screen_w, screen_h);
        
        SDL_RenderCopy(rend, screen_tex, NULL, &dst);
        if (vignette) SDL_RenderCopy(rend, vignette, NULL, &dst);
        
        int scan_alpha = 16 + (int)((1.0f - base) * 80.0f);
        if (scan_alpha < 8) scan_alpha = 8;
        draw_scanlines(rend, dst.x, dst.y, dst.w, dst.h, 3, scan_alpha);

        /* LED positions*/
        int led_r = 8;
        int led_spacing = 36;
        int center_x = screen_x + (screen_w / 2) - (led_spacing / 2);
        int leds_y = screen_y + screen_h + 24;

        /* clamp against actual renderer/window output height (not CRT height) */
        int out_w = 0, out_h = 0;
        SDL_GetRendererOutputSize(rend, &out_w, &out_h);
        if (leds_y + led_r + 4 > out_h) leds_y = out_h - led_r - 8;

        /* draw LEDs: green on when backend (wfd>=0) is running */
        draw_led(rend, center_x,               leds_y, led_r,  0, 220, 0, (wfd >= 0) ? 1 : 0);
        draw_led(rend, center_x + led_spacing, leds_y, led_r, 220,   0, 0, (wfd >= 0) ? 0 : 1);

        /* sticker anchored relative to LEDs exactly like main */
        int sticker_x = screen_x + screen_w - 180;
        int sticker_y = leds_y - 6;
        render_worn_sticker(rend, font, sticker_text, sticker_x, sticker_y);

        SDL_RenderPresent(rend);
        SDL_Delay(16);
    }
    
    lb_free(&ed);
    if (screen_tex) SDL_DestroyTexture(screen_tex);
    if (vignette) SDL_DestroyTexture(vignette);
}


/* ---------- NEW: Video Player with Pixel Engine and CRT FX ---------- */
static void run_video_viewer(const char *video_path, SDL_Window *win, SDL_Renderer *rend, TTF_Font *font,
                             int screen_x, int screen_y, int screen_w, int screen_h, const char *sticker_text, int wfd)
{

    // --- FFmpeg Initialization ---
    AVFormatContext *format_ctx = NULL;
    if (avformat_open_input(&format_ctx, video_path, NULL, NULL) != 0) {
        fprintf(stderr, "FFmpeg: Could not open video file %s\n", video_path);
        return;
    }
    if (avformat_find_stream_info(format_ctx, NULL) < 0) {
        fprintf(stderr, "FFmpeg: Could not find stream information\n");
        avformat_close_input(&format_ctx);
        return;
    }

    int video_stream_index = -1;
    AVCodecParameters *codec_params = NULL;
    for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            codec_params = format_ctx->streams[i]->codecpar;
            break;
        }
    }
    if (video_stream_index == -1) {
        fprintf(stderr, "FFmpeg: Could not find a video stream\n");
        avformat_close_input(&format_ctx);
        return;
    }

    AVCodec *codec = avcodec_find_decoder(codec_params->codec_id);
    if (!codec) { fprintf(stderr, "FFmpeg: Unsupported codec!\n"); avformat_close_input(&format_ctx); return; }
    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(codec_ctx, codec_params) < 0) {
        fprintf(stderr, "FFmpeg: Could not copy codec context\n");
        avcodec_free_context(&codec_ctx); avformat_close_input(&format_ctx); return;
    }
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "FFmpeg: Could not open codec\n");
        avcodec_free_context(&codec_ctx); avformat_close_input(&format_ctx); return;
    }


     // --- Find and Open Audio Stream ---
    int audio_stream_index = -1;
    AVCodecContext *audio_codec_ctx = NULL;
    for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_index = i;
            AVCodecParameters *audio_codec_params = format_ctx->streams[i]->codecpar;
            AVCodec *audio_codec = avcodec_find_decoder(audio_codec_params->codec_id);
            if (!audio_codec) break;
            audio_codec_ctx = avcodec_alloc_context3(audio_codec);
            if (avcodec_parameters_to_context(audio_codec_ctx, audio_codec_params) < 0 || avcodec_open2(audio_codec_ctx, audio_codec, NULL) < 0) {
                avcodec_free_context(&audio_codec_ctx);
                audio_codec_ctx = NULL;
            }
            break; // Found and opened audio stream
        }
    }

    // --- SDL Audio & FFmpeg Resampler Setup ---
    SDL_AudioDeviceID audio_dev = 0;
    struct SwrContext *swr_ctx_audio = NULL;

    if (audio_codec_ctx) {
        // Define target audio format: 8-bit unsigned, 44100 Hz, stereo
        SDL_AudioSpec want, have;
        av_opt_set_int(swr_ctx_audio = swr_alloc(), "in_channel_layout", audio_codec_ctx->channel_layout, 0);
        av_opt_set_int(swr_ctx_audio, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
        av_opt_set_int(swr_ctx_audio, "in_sample_rate", audio_codec_ctx->sample_rate, 0);
        av_opt_set_int(swr_ctx_audio, "out_sample_rate", 44100, 0);
        av_opt_set_sample_fmt(swr_ctx_audio, "in_sample_fmt", audio_codec_ctx->sample_fmt, 0);
        av_opt_set_sample_fmt(swr_ctx_audio, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
        swr_init(swr_ctx_audio);

        SDL_zero(want);
        want.freq = 44100;
        want.format = AUDIO_S16LSB;
        want.channels = 2;
        want.samples = 4096;
        want.callback = NULL; 

        audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (audio_dev == 0) {
            fprintf(stderr, "SDL_OpenAudioDevice Error: %s\n", SDL_GetError());
            swr_free(&swr_ctx_audio);
            avcodec_close(audio_codec_ctx);
            avcodec_free_context(&audio_codec_ctx);
            audio_codec_ctx = NULL;
        } else {
            SDL_PauseAudioDevice(audio_dev, 0); // Unpause audio device
        }
    }

    // --- Frame Conversion Setup (Video Format -> RGBA for SDL) ---
    struct SwsContext *sws_ctx = sws_getContext(
        codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
        codec_ctx->width, codec_ctx->height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, NULL, NULL, NULL
    );
    if (!sws_ctx) {
        fprintf(stderr, "FFmpeg: Could not create SwsContext\n");
        avcodec_close(codec_ctx); avcodec_free_context(&codec_ctx); avformat_close_input(&format_ctx); return;
    }

    uint8_t *audio_buf = NULL;
    if(audio_codec_ctx) {
         audio_buf = (uint8_t *)av_malloc(av_samples_get_buffer_size(NULL, 2, 4096, AV_SAMPLE_FMT_S16, 1));
    }

    // This SDL_Surface will hold the current converted video frame
    SDL_Surface *frame_surface = SDL_CreateRGBSurfaceWithFormat(0, codec_ctx->width, codec_ctx->height, 32, SDL_PIXELFORMAT_RGBA32);

    AVFrame *frame = av_frame_alloc();
    AVPacket packet;

    // --- CRT Rendering Setup ---
    SDL_Texture *screen_tex = SDL_CreateTexture(rend, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, screen_w - 16, screen_h - 16);
    if (screen_tex) SDL_SetTextureBlendMode(screen_tex, SDL_BLENDMODE_BLEND);
    SDL_Texture *vignette = create_vignette(rend, screen_w - 16, screen_h - 16);

    // --- Timing & Playback Loop ---
    double frame_rate = av_q2d(format_ctx->streams[video_stream_index]->r_frame_rate);
    double frame_delay = 1000.0 / frame_rate;
    Uint32 frame_timer = SDL_GetTicks();

    int playing = 1;
    while (playing && av_read_frame(format_ctx, &packet) >= 0) {
        if (packet.stream_index == video_stream_index) {
            if (avcodec_send_packet(codec_ctx, &packet) == 0) {
                while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                    // --- Frame is decoded, now handle events and render ---
                    SDL_Event ev;
                    while (SDL_PollEvent(&ev)) {
                        if (ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && (ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_q))) {
                            playing = 0;
                        }
                        // Handle resizing during playback
                         if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                             SDL_GetWindowSize(win, &screen_w, &screen_h);
                             if (screen_tex) SDL_DestroyTexture(screen_tex);
                             if (vignette) SDL_DestroyTexture(vignette);
                             screen_tex = SDL_CreateTexture(rend, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, screen_w - 16, screen_h - 16);
                             if (screen_tex) SDL_SetTextureBlendMode(screen_tex, SDL_BLENDMODE_BLEND);
                             vignette = create_vignette(rend, screen_w - 16, screen_h - 16);
                         }
                    }
                    if (!playing) break;

                    // Convert the frame to RGBA and put it in our SDL_Surface
                    SDL_LockSurface(frame_surface);
                    uint8_t *dst_pixels[4] = { (uint8_t*)frame_surface->pixels, NULL, NULL, NULL };
                    int dst_linesize[4] = { frame_surface->pitch, 0, 0, 0 };
                    sws_scale(sws_ctx, (const uint8_t * const *)frame->data, frame->linesize, 0, codec_ctx->height, dst_pixels, dst_linesize);
                    SDL_UnlockSurface(frame_surface);

                    // === START CRT COMPOSITING (identical to image viewer) ===
                    SDL_SetRenderTarget(rend, screen_tex);
                    SDL_SetRenderDrawColor(rend, 6, 18, 6, 255);
                    SDL_RenderClear(rend);

                    // Use our refactored function to draw the current video frame!
                    render_pixelated_surface_pass(rend, frame_surface, screen_w - 16, screen_h - 16);
                    render_text_with_glow(rend, font, "Playing video... Press ESC or Q to return.", 18, (screen_h - 16) - 40);

                    SDL_SetRenderTarget(rend, NULL);
                    SDL_SetRenderDrawColor(rend, 24, 20, 18, 255);
                    SDL_RenderClear(rend);
                    draw_frame(rend, screen_x, screen_y, screen_w, screen_h);

                    double now = SDL_GetTicks() / 1000.0;
                    float base = 0.90f + 0.08f * sinf((float)(now * 6.0));
                    if ((rand() % 1000) < 6) base -= ((rand() % 40) / 255.0f);
                    if (base < 0.5f) base = 0.5f;
                    if (base > 1.05f) base = 1.05f;
                    SDL_SetTextureAlphaMod(screen_tex, (Uint8)(base * 255.0f));
                    SDL_SetTextureColorMod(screen_tex, 180, 200 + (int)(55.0f * (base - 0.9f) * 4.0f), 180);

                    SDL_Rect dst = { screen_x + 8, screen_y + 8, screen_w - 16, screen_h - 16 };
                    SDL_RenderCopy(rend, screen_tex, NULL, &dst);
                    if (vignette) SDL_RenderCopy(rend, vignette, NULL, &dst);
                    draw_scanlines(rend, dst.x, dst.y, dst.w, dst.h, 3, 16 + (int)((1.0f - base) * 80.0f));
                    
                    render_worn_sticker(rend, font, sticker_text, screen_x + screen_w - 180, screen_y + screen_h + 18);
                    
                    SDL_RenderPresent(rend);
                    // === END CRT COMPOSITING ===

                    // --- Timing logic to play at the correct speed ---
                    while (SDL_GetTicks() < frame_timer + frame_delay) {
                        SDL_Delay(1);
                    }
                    frame_timer = SDL_GetTicks();
                }
            }
        }

        else if (packet.stream_index == audio_stream_index && audio_codec_ctx) {
            if (avcodec_send_packet(audio_codec_ctx, &packet) == 0) {
                while (avcodec_receive_frame(audio_codec_ctx, frame) == 0) {
                    int samples_converted = swr_convert(swr_ctx_audio, &audio_buf, 4096, (const uint8_t **)frame->data, frame->nb_samples);
                    if (samples_converted > 0) {
                        int out_size = av_samples_get_buffer_size(NULL, 2, samples_converted, AV_SAMPLE_FMT_U8, 1);
                        SDL_QueueAudio(audio_dev, audio_buf, out_size);
                    }
                }
            }
        }
        av_packet_unref(&packet);
        if (!playing) break;
    }

    // --- Cleanup ---

    av_frame_free(&frame);
    if (audio_buf) av_free(audio_buf); 
    SDL_FreeSurface(frame_surface);
    sws_freeContext(sws_ctx);
    avcodec_close(codec_ctx);


    
    if (audio_codec_ctx) {
        if (audio_dev > 0) SDL_CloseAudioDevice(audio_dev);
        if (swr_ctx_audio) swr_free(&swr_ctx_audio);
        avcodec_close(audio_codec_ctx);
        avcodec_free_context(&audio_codec_ctx);
    }   

    avcodec_free_context(&codec_ctx);
    avformat_close_input(&format_ctx);
    SDL_DestroyTexture(screen_tex);
    SDL_DestroyTexture(vignette);
}


/* ---------- NEW: Pixel Art Image Viewer (with full CRT effects) ---------- */
static void run_image_viewer(const char *image_path, SDL_Renderer *rend, TTF_Font *font,
                             int screen_x, int screen_y, int screen_w, int screen_h)
{
    // --- Start of changes ---
    // Step 1: Create the offscreen texture for our CRT content and a vignette.
    // This is crucial for applying the CRT effects correctly.
    SDL_Texture *screen_tex = SDL_CreateTexture(rend, SDL_PIXELFORMAT_RGBA8888,
                                                SDL_TEXTUREACCESS_TARGET, screen_w - 16, screen_h - 16);
    if (screen_tex) SDL_SetTextureBlendMode(screen_tex, SDL_BLENDMODE_BLEND);
    SDL_Texture *vignette = create_vignette(rend, screen_w - 16, screen_h - 16);
    // --- End of changes ---

    SDL_Surface *original_surface = IMG_Load(image_path);
    if (!original_surface) {
        fprintf(stderr, "IMG_Load Error: %s\n", IMG_GetError());
        // Clean up textures if image fails to load
        if (screen_tex) SDL_DestroyTexture(screen_tex);
        if (vignette) SDL_DestroyTexture(vignette);
        return;
    }

    SDL_Surface* formatted_surface = SDL_ConvertSurfaceFormat(original_surface, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(original_surface);
    if (!formatted_surface) {
        fprintf(stderr, "SDL_ConvertSurfaceFormat Error: %s\n", SDL_GetError());
        if (screen_tex) SDL_DestroyTexture(screen_tex);
        if (vignette) SDL_DestroyTexture(vignette);
        return;
    }

    const SDL_Color green_palette[] = {
        {10, 45, 10, 255},
        {30, 110, 30, 255},
        {51, 205, 51, 255},
        {140, 255, 140, 255},
        {210, 255, 210, 255}
    };
    const int palette_size = sizeof(green_palette) / sizeof(green_palette[0]);

    const int PIXEL_COLS = 128;
    const int PIXEL_ROWS = 96;

    int viewing = 1;
    while (viewing) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) || (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_q)) {
                viewing = 0;
            }
            if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                 SDL_GetWindowSize(SDL_GetWindowFromID(ev.window.windowID), &screen_w, &screen_h);
                 // Recreate textures on resize
                 if (screen_tex) SDL_DestroyTexture(screen_tex);
                 if (vignette) SDL_DestroyTexture(vignette);
                 screen_tex = SDL_CreateTexture(rend, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, screen_w - 16, screen_h - 16);
                 if (screen_tex) SDL_SetTextureBlendMode(screen_tex, SDL_BLENDMODE_BLEND);
                 vignette = create_vignette(rend, screen_w - 16, screen_h - 16);
            }
        }

        // --- Rendering ---
        
        // --- Start of changes ---
        // Step 2: Set the render target to our offscreen texture.
        // All subsequent drawing will go to this texture, not the screen.
        if (screen_tex) SDL_SetRenderTarget(rend, screen_tex);
        
        // Clear the texture with the dark green CRT background color
        SDL_SetRenderDrawColor(rend, 6, 18, 6, 255);
        SDL_RenderClear(rend);
        // --- End of changes ---

        // Calculate the size of each rendered "pixel" block
        float pixel_block_w = (float)(screen_w - 16) / PIXEL_COLS;
        float pixel_block_h = (float)(screen_h - 16) / PIXEL_ROWS;

        Uint32 *pixels = (Uint32 *)formatted_surface->pixels;
        int src_w = formatted_surface->w;
        int src_h = formatted_surface->h;

        for (int y = 0; y < PIXEL_ROWS; ++y) {
            for (int x = 0; x < PIXEL_COLS; ++x) {
                int src_x_start = (int)((float)x / PIXEL_COLS * src_w);
                int src_y_start = (int)((float)y / PIXEL_ROWS * src_h);
                int src_x_end = (int)((float)(x + 1) / PIXEL_COLS * src_w);
                int src_y_end = (int)((float)(y + 1) / PIXEL_ROWS * src_h);

                double total_brightness = 0;
                int sample_count = 0;
                for (int sy = src_y_start; sy < src_y_end; ++sy) {
                    for (int sx = src_x_start; sx < src_x_end; ++sx) {
                        Uint32 pixel_data = pixels[sy * src_w + sx];
                        Uint8 r, g, b, a;
                        SDL_GetRGBA(pixel_data, formatted_surface->format, &r, &g, &b, &a);
                        double brightness = (0.299 * r + 0.587 * g + 0.114 * b) / 255.0;
                        total_brightness += brightness;
                        sample_count++;
                    }
                }
                
                if (sample_count == 0) continue;
                double avg_brightness = total_brightness / sample_count;

                int palette_index = (int)(avg_brightness * (palette_size)); // Use full range
                if (palette_index < 0) palette_index = 0;
                if (palette_index >= palette_size) palette_index = palette_size - 1;
                SDL_Color final_color = green_palette[palette_index];

                SDL_Rect pixel_rect = {
                    (int)(x * pixel_block_w),
                    (int)(y * pixel_block_h),
                    // Fix: Use ceil() to ensure blocks slightly overlap and prevent gaps.
                    (int)ceil(pixel_block_w),
                    (int)ceil(pixel_block_h)
                };
                SDL_SetRenderDrawColor(rend, final_color.r, final_color.g, final_color.b, 255);
                SDL_RenderFillRect(rend, &pixel_rect);
            }
        }
        
        // Add the exit message inside the texture so it gets the CRT effects too
        render_text_with_glow(rend, font, "Displaying image... Press ESC or Q to return.", 20, (screen_h - 16) - 30);
        
        // --- Start of changes ---
        // Step 3: Switch back to the main renderer.
        SDL_SetRenderTarget(rend, NULL);

        // Step 4: Composite the CRT texture with all the effects, just like the main loop.
        // (This block is copied and adapted from the main rendering loop)
        SDL_SetRenderDrawColor(rend, 24, 20, 18, 255);
        SDL_RenderClear(rend);
        draw_frame(rend, screen_x, screen_y, screen_w, screen_h);
        
        // Apply flicker & jitter
        double now = SDL_GetTicks() / 1000.0;
        float base = 0.90f + 0.08f * sinf((float)(now * 6.0));
        if ((rand() % 1000) < 6) base -= ((rand() % 40) / 255.0f);
        if (base < 0.5f) base = 0.5f;
        if (base > 1.05f) base = 1.05f;
        Uint8 alpha = (Uint8)(base * 255.0f);
        SDL_SetTextureAlphaMod(screen_tex, alpha);
        
        int green_mod = 180 + (int)(75.0f * (base - 0.9f) * 4.0f);
        if (green_mod < 180) green_mod = 180;
        if (green_mod > 255) green_mod = 255;
        SDL_SetTextureColorMod(screen_tex, 180, green_mod, 180);

        int jitter_x = 0, jitter_y = 0;
        if ((rand() % 100) < 8) { jitter_x = (rand() % 3) - 1; jitter_y = (rand() % 3) - 1; }

        SDL_Rect dst = { screen_x + 8 + jitter_x, screen_y + 8 + jitter_y, screen_w - 16, screen_h - 16 };
        
        // Blit the final CRT content texture
        SDL_RenderCopy(rend, screen_tex, NULL, &dst);

        // Overlay vignette and scanlines
        if (vignette) SDL_RenderCopy(rend, vignette, NULL, &dst);
        int scan_alpha = 16 + (int)((1.0f - base) * 80.0f);
        draw_scanlines(rend, dst.x, dst.y, dst.w, dst.h, 3, scan_alpha);
        // --- End of changes ---
        
        SDL_RenderPresent(rend);
        SDL_Delay(16);
    }

    // Clean up all resources
    SDL_FreeSurface(formatted_surface);
    if (screen_tex) SDL_DestroyTexture(screen_tex);
    if (vignette) SDL_DestroyTexture(vignette);
}

/* --- main --- */
int main(int argc, char **argv) {
    
    char full_backend_path[PATH_MAX]; // For the path to the backend executable
    char backend_cwd[PATH_MAX];       // For the backend's working directory
    char exe_path[PATH_MAX];          // To store the frontend's executable path
    char current_username[MAX_USERNAME] = "guest"; // Default to guest

    // 1. Get the path of the currently running frontend executable
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len == -1) {
        // Fallback if readlink fails (e.g., non-Linux system or permissions issue)
        fprintf(stderr, "Warning: Could not determine executable path. Falling back to relative paths.\n");
        // If we can't determine the path, we fall back to assuming current directory
        snprintf(full_backend_path, sizeof(full_backend_path), "./bin/cortez_backend");
        snprintf(backend_cwd, sizeof(backend_cwd), "."); // Default to current directory
    } else {
        exe_path[len] = '\0'; // Null-terminate the path

        // 2. Determine the root of the mounted filesystem
        // Find the last '/' to get the directory containing the executable (e.g., /home/user/myfs/bin)
        char *bin_dir_end = strrchr(exe_path, '/');
        if (bin_dir_end != NULL) {
            *bin_dir_end = '\0'; // Terminate at /bin to get /home/user/myfs

            // Now, `exe_path` contains the path to the `bin` directory (e.g., /home/user/myfs/bin)
            // We need to go up one level to get the root of the mounted filesystem (e.g., /home/user/myfs)
            char *root_dir_end = strrchr(exe_path, '/');
            if (root_dir_end != NULL) {
                *root_dir_end = '\0'; // Terminate to get the root directory
                snprintf(backend_cwd, sizeof(backend_cwd), "%s", exe_path); // This is the root (e.g., /home/user/myfs)
            } else {
                // If no parent directory found (e.g., /bin), treat current directory as root
                snprintf(backend_cwd, sizeof(backend_cwd), ".");
            }

            // 3. Construct the full path to the backend executable relative to the root
            // Since `exe_path` now holds the root, we append /bin/cortez_backend
            snprintf(full_backend_path, sizeof(full_backend_path), "%s/bin/cortez_backend", backend_cwd);

        } else {
            // Should not happen if run from a path with slashes, but fallback
            snprintf(full_backend_path, sizeof(full_backend_path), "./bin/cortez_backend");
            snprintf(backend_cwd, sizeof(backend_cwd), ".");
        }
    }
    

    if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER|SDL_INIT_AUDIO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    if (TTF_Init() != 0) { fprintf(stderr, "TTF_Init: %s\n", TTF_GetError()); SDL_Quit(); return 1; }

    LineBuf lb;

    int flags = MIX_INIT_MP3 | MIX_INIT_OGG | MIX_INIT_FLAC;
    int initted = Mix_Init(flags);
    if ((initted & flags) != flags) {
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg), "[audio-init-err] Mix_Init: Failed to init required loaders! Error: %s", Mix_GetError());
        lb_push(&lb, err_msg);
        fprintf(stderr, "Mix_Init: Failed to init required ogg, flac, and mp3 support!\nMix_Init Error: %s\n", Mix_GetError());
    }

    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0) {
    char err_msg[512]; 
    snprintf(err_msg, sizeof(err_msg), "[audio-init-err] %s", Mix_GetError()); 
    lb_push(&lb, err_msg); 
    
    fprintf(stderr, "SDL_mixer could not initialize! Mix_Error: %s\n", Mix_GetError());
    }

    // create window then go fullscreen desktop for consistent full-screen sizing
    SDL_Window *win = SDL_CreateWindow("Cortez Terminal (CRT)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); TTF_Quit(); SDL_Quit(); return 1; }
    if (SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) {
        fprintf(stderr, "SDL_SetWindowFullscreen: %s\n", SDL_GetError());
    }

    SDL_Renderer *rend = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!rend) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); SDL_DestroyWindow(win); TTF_Quit(); SDL_Quit(); return 1; }

    // font candidates
    const char *candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
        "/Library/Fonts/Andale Mono.ttf",
        "/usr/local/share/fonts/DejaVuSansMono.ttf",
        NULL
    };
    const char *font_path = NULL;
    for (int i=0; candidates[i]; ++i) {
        if (access(candidates[i], R_OK) == 0) { font_path = candidates[i]; break; }
    }
    if (!font_path) {
        fprintf(stderr, "No monospace TTF found. Install DejaVuSansMono or adjust source.\n");
        SDL_DestroyRenderer(rend); SDL_DestroyWindow(win); TTF_Quit(); SDL_Quit(); return 1;
    }
    TTF_Font *font = TTF_OpenFont(font_path, FONT_SIZE);
    if (!font) { fprintf(stderr, "TTF_OpenFont: %s\n", TTF_GetError()); SDL_DestroyRenderer(rend); SDL_DestroyWindow(win); TTF_Quit(); SDL_Quit(); return 1; }

    SDL_StartTextInput();
    SDL_Color fg = {51,255,51,255};

     lb_init(&lb);

    int win_w = 1280, win_h = 720;
    SDL_GetWindowSize(win, &win_w, &win_h);

    // compute CRT screen rect as percentage of window so it scales nicely
    float pad_pct = 0.06f; // 6% padding each side
    int screen_x = (int)(win_w * pad_pct);
    int screen_y = (int)(win_h * pad_pct * 0.6f); // top padding slightly smaller
    int screen_w = win_w - 2 * screen_x;
    int screen_h = win_h - screen_y - (int)(win_h * pad_pct); // bottom padding
    if (screen_w < 200) screen_w = win_w - 2*screen_x;
    if (screen_h < 160) screen_h = win_h - screen_y - (int)(win_h * pad_pct);

    // create render target for CRT screen content (inner area)
    SDL_Texture *screen_tex = SDL_CreateTexture(rend, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, screen_w - 16, screen_h - 16);
    if (screen_tex) { SDL_SetTextureBlendMode(screen_tex, SDL_BLENDMODE_BLEND); }

    // create vignette texture for current screen size
    SDL_Texture *vignette = create_vignette(rend, screen_w - 16, screen_h - 16);

    // draw background and initial frame before animation
    SDL_SetRenderDrawColor(rend, 24, 20, 18, 255);
    SDL_RenderClear(rend);
    draw_frame(rend, screen_x, screen_y, screen_w, screen_h);
    SDL_RenderPresent(rend);

    // slowly reveal ART_LINES inside the CRT render target
    int art_x = 28;
    int art_y = 18;
    for (int i = 0; ART_LINES[i]; ++i) {
        const char *line = ART_LINES[i];
        size_t len = strlen(line);
        char *buf = malloc(len + 1);
        if (!buf) break;
        for (size_t k=0;k<len;k++) buf[k] = ' ';
        buf[len] = 0;
        for (size_t c=0;c<len;c++) {
            buf[c] = line[c];

            // render into screen_tex (so it gets vignette / scanlines / flicker later)
            if (screen_tex) SDL_SetRenderTarget(rend, screen_tex);
            SDL_SetRenderDrawColor(rend, 6, 18, 6, 255); // deep greenish inside screen
            SDL_RenderClear(rend);

            // previously filled art lines
            int yy = art_y;
            for (int j=0;j<i;++j) {
                if (!ART_LINES[j]) continue;
                SDL_Surface *s = TTF_RenderUTF8_Blended(font, ART_LINES[j], fg);
                if (s) {
                    SDL_Texture *t = SDL_CreateTextureFromSurface(rend, s);
                    if (t) {
                        SDL_Rect dst = { art_x, yy, s->w, s->h };
                        SDL_RenderCopy(rend, t, NULL, &dst);
                        SDL_DestroyTexture(t);
                    }
                    SDL_FreeSurface(s);
                }
                yy += FONT_SIZE + LINE_SPACING;
            }
            // current partial line
            SDL_Surface *cs = TTF_RenderUTF8_Blended(font, buf, fg);
            if (cs) {
                SDL_Texture *ct = SDL_CreateTextureFromSurface(rend, cs);
                if (ct) {
                    SDL_Rect dst = { art_x, yy, cs->w, cs->h };
                    SDL_RenderCopy(rend, ct, NULL, &dst);
                    SDL_DestroyTexture(ct);
                }
                SDL_FreeSurface(cs);
            }

            // done drawing into target, switch back to default and composite
            SDL_SetRenderTarget(rend, NULL);

            // draw the static frame (non-flickering)
            SDL_SetRenderDrawColor(rend, 24, 20, 18, 255);
            SDL_RenderClear(rend);
            draw_frame(rend, screen_x, screen_y, screen_w, screen_h);

            // compute jitter & flicker for animation (small)
            double now = SDL_GetTicks() / 1000.0;
            float flick = 0.96f + 0.04f * sinf((float)(now * 12.0 + (float)c * 0.3f));
            Uint8 alpha = (Uint8)(flick * 255.0f);
            SDL_SetTextureAlphaMod(screen_tex, alpha);

            // blit screen_tex into scr area
            SDL_Rect dst = { screen_x + 8, screen_y + 8, screen_w - 16, screen_h - 16 };
            SDL_RenderCopy(rend, screen_tex, NULL, &dst);

            // vignette on top of inner screen
            if (vignette) SDL_RenderCopy(rend, vignette, NULL, &dst);

            // subtle animated scanlines (alpha modulated)
            int scan_alpha = 18 + (rand() % 8);
            draw_scanlines(rend, dst.x, dst.y, dst.w, dst.h, 3, scan_alpha);

            int led_r = 8;
                int led_spacing = 36;
                int center_x = screen_x + (screen_w / 2) - (led_spacing / 2);

                /* put LEDs a bit lower during animation */
                int leds_y = screen_y + screen_h + 24;    /* lowered a little compared to +12 */
                if (leds_y + led_r + 4 > win_h) leds_y = win_h - led_r - 8;

                /* green off (0), red on (1) while loading */
                draw_led(rend, center_x,               leds_y, led_r,  0, 220, 0, 0);
                draw_led(rend, center_x + led_spacing, leds_y, led_r, 220,   0, 0, 1);

             
            int sticker_x = screen_x + screen_w - 180; /* tweak -180 to move further left/right */
            int sticker_y = leds_y - 6;                /* aligns roughly with LEDs baseline */
            render_worn_sticker(rend, font, sticker_text, sticker_x, sticker_y);
        

            SDL_RenderPresent(rend);

            SDL_Delay(6);
            // keep window responsive
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) { free(buf); goto after_anim; }
                else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                    SDL_GetWindowSize(win, &win_w, &win_h);
                    screen_x = (int)(win_w * pad_pct);
                    screen_y = (int)(win_h * pad_pct * 0.6f);
                    screen_w = win_w - 2 * screen_x;
                    screen_h = win_h - screen_y - (int)(win_h * pad_pct);
                    if (screen_w < 200) screen_w = win_w - 2*screen_x;
                    if (screen_h < 160) screen_h = win_h - screen_y - (int)(win_h * pad_pct);

                    if (screen_tex) { SDL_DestroyTexture(screen_tex); screen_tex = SDL_CreateTexture(rend, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, screen_w - 16, screen_h - 16); if (screen_tex) SDL_SetTextureBlendMode(screen_tex, SDL_BLENDMODE_BLEND); }
                    if (vignette) { SDL_DestroyTexture(vignette); vignette = create_vignette(rend, screen_w - 16, screen_h - 16); }
                    art_x = 28;
                    art_y = 18;
                }
            }
        }
        free(buf);
        SDL_Delay(30);
    }
after_anim:

    /* Attempt to set cwd to project root (the directory containing bin/).
       This makes data/ and home/ siblings of bin/ and avoids ../ ambiguity. */
do {
    char exe_path[PATH_MAX] = {0};
    ssize_t l = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (l > 0) exe_path[l] = '\0';
    char *p = strstr(exe_path, "/bin/");
    if (p) {
        *p = '\0';
        if (chdir(exe_path) != 0) {
            /* non-fatal; continue with current cwd */
        }
    } else {
        /* fallback: try parent of exe */
        char *last = strrchr(exe_path, '/');
        if (last) {
            *last = '\0';
            (void)chdir(exe_path);
        }
    }
} while (0);


    /* Launch login in its own window (blocking until login success or cancel).
       NOTE: frontend already does backend work elsewhere — we do NOT start/modify backend here. */
    {
        int login_w = 900;
        int login_h = 500;

        /* popup/modal in the same window: render login overlay onto the main renderer */
char *logged_user = run_login_modal_on_renderer(rend, font, win_w, win_h);


        if (logged_user) {
            snprintf(current_username, sizeof(current_username), "%s", logged_user);
            /* Set sticker and personalized welcome */
            snprintf(sticker_text, sizeof(sticker_text), "%s Tech", logged_user);

            lb_push(&lb, "");
            char bufw[256];
            snprintf(bufw, sizeof(bufw), "Welcome to %s", sticker_text);
            lb_push(&lb, bufw);
            lb_push(&lb, "Type 'help' and press Enter.");
            lb_push(&lb, "");

            /* create and chdir to user's home using frontend-provided posix_fs adapter */
            if (create_home_dir_and_chdir(logged_user, &posix_fs) != 0) {
                fprintf(stderr, "Warning: create_home_dir_and_chdir failed for %s\n", logged_user);
                /* still continue - user's working dir may remain project root */
            }

            free(logged_user);
        } else {
            /* login cancelled or failed */
            lb_push(&lb, "");
            lb_push(&lb, "Welcome to Cortez Terminal (CRT)");
            lb_push(&lb, "Type 'help' and press Enter.");
            lb_push(&lb, "");
        }
    }

SDL_StartTextInput();


    int wfd=-1, rfd=-1, efd=-1; pid_t backend_pid=-1;
    // indicate starting backend inside the CRT buffer
    lb_push(&lb, "[starting backend...]");
    if (access(full_backend_path, X_OK) == 0) {
        backend_pid = spawn_backend(full_backend_path, &wfd, &rfd, &efd);;
        if (backend_pid <= 0) {
            lb_push(&lb, "[failed to start backend]");
            backend_pid = -1;
        } else {
            SDL_Delay(120);
            lb_push(&lb, "[backend started]");
        }
    } else {
        lb_push(&lb, "[backend binary not found or not executable: build it with 'make' in src/]");
    }

    char inputbuf[8192] = {0}; size_t ipos = 0; int scroll = 0; int running = 1;
    char proto[32768]; size_t proto_len = 0;

    // main loop
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                SDL_GetWindowSize(win, &win_w, &win_h);
                screen_x = (int)(win_w * pad_pct);
                screen_y = (int)(win_h * pad_pct * 0.6f);
                screen_w = win_w - 2 * screen_x;
                screen_h = win_h - screen_y - (int)(win_h * pad_pct);
                if (screen_w < 200) screen_w = win_w - 2*screen_x;
                if (screen_h < 160) screen_h = win_h - screen_y - (int)(win_h * pad_pct);
                if (screen_tex) { SDL_DestroyTexture(screen_tex); screen_tex = SDL_CreateTexture(rend, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, screen_w - 16, screen_h - 16); if (screen_tex) SDL_SetTextureBlendMode(screen_tex, SDL_BLENDMODE_BLEND); }
                if (vignette) { SDL_DestroyTexture(vignette); vignette = create_vignette(rend, screen_w - 16, screen_h - 16); }
            } else if (ev.type == SDL_TEXTINPUT) {
                const char *txt = ev.text.text;
                for (int i=0; txt[i]; ++i) { if (ipos + 1 < (int)sizeof(inputbuf)) { inputbuf[ipos++] = txt[i]; inputbuf[ipos]=0; } }
            } else if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode k = ev.key.keysym.sym;
                if (k == SDLK_BACKSPACE) { if (ipos > 0) { ipos--; inputbuf[ipos]=0; } }
                else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    if (ipos > 0) {
                        inputbuf[ipos]=0;

                       if (strncmp(inputbuf, "cedit ", 6) == 0) {
    const char *fn = inputbuf + 6;

    // Skip leading whitespace from the filename
    while (*fn == ' ') {
        fn++;
    }

    // Create a temporary buffer to hold a potentially cleaned-up filename
    char cleaned_fn[PATH_MAX];
    strncpy(cleaned_fn, fn, sizeof(cleaned_fn) - 1);
    cleaned_fn[sizeof(cleaned_fn) - 1] = '\0';

    // Check for and remove the "[ok] " prefix
    if (strncmp(cleaned_fn, "[ok] ", 5) == 0) {
        // Move the rest of the string to the beginning
        memmove(cleaned_fn, cleaned_fn + 5, strlen(cleaned_fn + 5) + 1);
    }

    run_simple_editor(cleaned_fn, rend, font, screen_x, screen_y, screen_w, screen_h, backend_cwd, (backend_pid > 0) ? 1 : 0);
    /* after editor returns, don't send to backend */
} else if (strcmp(inputbuf, "cedit") == 0) {
    /*typed just 'cedit' without filename -> open empty scratch */
       run_simple_editor("Untitled.txt", rend, font, screen_x, screen_y, screen_w, screen_h, backend_cwd, (backend_pid > 0) ? 1 : 0);
}else {
                        /* normal behavior: forward to backend */
                            if (wfd >= 0) backend_write_line(wfd, inputbuf);
                                 else { lb_push(&lb, "[no backend]"); lb_push(&lb, inputbuf); }
    }
    ipos = 0;
} else {
    if (wfd >= 0) backend_write_line(wfd, "");
}

                } else if (k == SDLK_c && (ev.key.keysym.mod & KMOD_CTRL)) {
                    if (wfd >= 0) backend_write_line(wfd, "SIGINT");
                } else if (k == SDLK_UP) { scroll++; }
                else if (k == SDLK_DOWN) { if (scroll>0) scroll--; }
            }
        }

        // read backend fds (same improved handling)
        if (rfd >= 0) {
            char buf[4096];
            ssize_t rd = read(rfd, buf, sizeof(buf));
            if (rd > 0) {
                if (proto_len + rd < sizeof(proto)-1) {
                    memcpy(proto + proto_len, buf, rd); proto_len += rd; proto[proto_len]=0;
                    char *nl;
                    while ((nl = memchr(proto, '\n', proto_len)) != NULL) {
                        size_t linelen = nl - proto;
                        if (linelen >= 16383) linelen = 16383;
                        char line[16384]; memcpy(line, proto, linelen); line[linelen]=0;
                        size_t remain = proto_len - (linelen + 1);
                        memmove(proto, nl+1, remain); proto_len = remain; proto[proto_len]=0;

                        int approx_char_w = (int)(FONT_SIZE * 0.6f);
                        if (approx_char_w <= 0) approx_char_w = 8;
                        int maxcols = (screen_w - 40) / approx_char_w;
                        if (maxcols < 40) maxcols = 40;
                        if (maxcols > 1000) maxcols = 1000;

if (strncmp(line, "OK cd", 5) == 0) {
    // The line before a successful 'cd' is the new directory
    if (lb.count > 0) {
        const char* path_line = lb.lines[lb.count - 1];
        // FIX: Check for and skip the "OUT " prefix
        if (strncmp(path_line, "OUT ", 4) == 0) {
            strncpy(backend_cwd, path_line + 4, PATH_MAX - 1);
        } else {
            // Fallback for safety, copy the whole line if no prefix
            strncpy(backend_cwd, path_line, PATH_MAX - 1);
        }
        backend_cwd[PATH_MAX - 1] = '\0';
    }
}

                        if (strncmp(line, "OK ", 3) == 0) {
                            const char *payload = line + 3;
                            size_t n = strlen(payload) + 6;
                            char *tmp = malloc(n);
                            if (tmp) { snprintf(tmp, n, "[ok] %s", payload); lb_push_wrapped(&lb, tmp, maxcols); free(tmp); }
                        } else if (strncmp(line, "ERR ", 4) == 0) {
                            const char *payload = line + 4;
                            size_t n = strlen(payload) + 7;
                            char *tmp = malloc(n);
                            if (tmp) { snprintf(tmp, n, "[err] %s", payload); lb_push_wrapped(&lb, tmp, maxcols); free(tmp); }
                        }else if (strncmp(line, "CMD_SHOW_IMAGE ", 15) == 0) {
                            const char *image_path = line + 15;
                            run_image_viewer(image_path, rend, font, screen_x, screen_y, screen_w, screen_h);
                        }else if (strncmp(line, "CMD_SHOW_VIDEO ", 15) == 0) { 
                            const char *video_path = line + 15;
                            run_video_viewer(video_path, win, rend, font, screen_x, screen_y, screen_w, screen_h, sticker_text,wfd);    
                        }else if (strncmp(line, "CMD_PLAY_AUDIO ", 15) == 0) { // <-- ADD THIS BLOCK
                            const char *audio_path = line + 15;
                            play_audio_in_frontend(audio_path, &lb);
                        } else if (strncmp(line, "STREAM_START ", 13) == 0) {
                            const char *payload = line + 13;
                            size_t n = strlen(payload) + 20;
                            char *tmp = malloc(n);
                            if (tmp) { snprintf(tmp, n, "[stream start %s]", payload); lb_push_wrapped(&lb, tmp, maxcols); free(tmp); }
                        } else if (strncmp(line, "STREAM_DATA ", 12) == 0) {
                            const char *b64 = line + 12; unsigned char dec[16384];
                            size_t dsz = b64_decode(b64, dec, sizeof(dec));
                            if (dsz) {
                                char *tmp = malloc(dsz + 1);
                                if (tmp) {
                                    memcpy(tmp, dec, dsz);
                                    tmp[dsz] = 0;

                                    strip_ansi_sequences(tmp);

                                    /* strip/handle ANSI clear sequences so we actually clear the frontend view */
                                    const char *seqs[] = { "\x1b[2J\x1b[H", "\x1b[2J", "\x1b[H", NULL };
                                    for (int si = 0; seqs[si]; ++si) {
                                        char *pos;
                                        while ((pos = strstr(tmp, seqs[si])) != NULL) {
                                        /* clear the buffer and remove the sequence from tmp */
                                        lb_clear(&lb);
                                        size_t sl = strlen(seqs[si]);
                                        size_t tail = strlen(pos + sl);
                                        memmove(pos, pos + sl, tail + 1);
                                        }
                            }

        /* now push any remaining printable text, split on newlines */
        char *p = tmp; char *q;
        while ((q = strchr(p, '\n')) != NULL) { *q = 0; if (*p) lb_push_wrapped(&lb, p, maxcols); p = q+1; }
        if (*p) lb_push_wrapped(&lb, p, maxcols);

        free(tmp);
    }
}

                        } else if (strncmp(line, "CMD_EDIT_NODE ", 14) == 0) {
                            int node_id = atoi(line + 14);
                            LineBuf node_buf; lb_init(&node_buf);
                            // Read subsequent lines until CMD_EDIT_END
                            while (1) {
                                // We need to read from rfd directly or parse from proto?
                                // The main loop parses proto. We are inside the loop.
                                // We need to break out and enter a buffering mode?
                                // Or we can just continue reading here (blocking the main loop slightly, but okay for this).
                                // BUT, we need to handle partial reads.
                                // Re-using the main loop's buffer is hard.
                                // Let's just assume we can read line by line from proto if available, or read more.
                                // Actually, simpler: Just switch to a state "BUFFERING_NODE" in the main loop?
                                // But that requires refactoring the loop.
                                // Let's try to read lines here.
                                
                                // Wait, we have `proto` buffer.
                                // We can check if `CMD_EDIT_END` is in `proto`.
                                // If not, read more.
                                
                                char *nl = memchr(proto, '\n', proto_len);
                                while (!nl) {
                                    int n = read(rfd, buf, sizeof(buf));
                                    if (n <= 0) break;
                                    if (proto_len + n < sizeof(proto)-1) {
                                        memcpy(proto + proto_len, buf, n); proto_len += n; proto[proto_len]=0;
                                    }
                                    nl = memchr(proto, '\n', proto_len);
                                }
                                if (!nl) break; // Should not happen unless connection lost
                                
                                size_t linelen = nl - proto;
                                char l[16384]; memcpy(l, proto, linelen); l[linelen]=0;
                                size_t remain = proto_len - (linelen + 1);
                                memmove(proto, nl+1, remain); proto_len = remain; proto[proto_len]=0;
                                
                                if (strncmp(l, "CMD_EDIT_END", 12) == 0) break;
                                lb_push(&node_buf, l);
                            }
                            run_node_editor(node_id, &node_buf, rend, font, screen_x, screen_y, screen_w, screen_h, wfd, sticker_text);
                            lb_free(&node_buf);
                        }else if (strncmp(line, "STREAM_END ", 11) == 0) {
                            const char *payload = line + 11;
                            size_t n = strlen(payload) + 16;
                            char *tmp = malloc(n);
                            if (tmp) { snprintf(tmp, n, "[stream end %s]", payload); lb_push_wrapped(&lb, tmp, maxcols); free(tmp); }
                        } else {
    /* if the backend emitted raw ANSI clear as an OUT line, treat it as wipe */
    if (strstr(line, "\x1b[2J") || strstr(line, "\x1b[H")) {
        lb_clear(&lb);
        /* remove the sequences and push any remaining text */
        char *tmp = strdup(line);
        const char *seqs[] = { "\x1b[2J\x1b[H", "\x1b[2J", "\x1b[H", NULL };
        for (int si = 0; seqs[si]; ++si) {
            char *pos;
            while ((pos = strstr(tmp, seqs[si])) != NULL) {
                size_t sl = strlen(seqs[si]); size_t tail = strlen(pos + sl);
                memmove(pos, pos + sl, tail + 1);
            }
        }
        if (strlen(tmp) > 0) lb_push_wrapped(&lb, tmp, maxcols);
        free(tmp);
    } else {
        lb_push_wrapped(&lb, line, maxcols);
    }
}

                    }
                } else { proto_len = 0; }
            } else if (rd == 0) { lb_push(&lb, "[backend closed]"); close(rfd); rfd = -1; }
        }
        if (efd >= 0) {
            char ebuf[512]; ssize_t er = read(efd, ebuf, sizeof(ebuf)-1);
            if (er > 0) { ebuf[er]=0; char tmp[1024]; snprintf(tmp,sizeof(tmp), "[backend-stderr] %s", ebuf); lb_push(&lb, tmp); }
            else if (er == 0) { close(efd); efd = -1; }
        }

        // Render main scene
        SDL_SetRenderDrawColor(rend, 24, 20, 18, 255);
        SDL_RenderClear(rend);

        // recompute screen rect (safety)
        screen_w = win_w - 2 * screen_x;
        screen_h = win_h - screen_y - (int)(win_h * pad_pct);
        if (screen_w < 200) screen_w = win_w - 2*screen_x;
        if (screen_h < 160) screen_h = win_h - screen_y - (int)(win_h * pad_pct);

        // draw static frame (no flicker)
        draw_frame(rend, screen_x, screen_y, screen_w, screen_h);

        // draw CRT contents into screen_tex
        if (screen_tex) SDL_SetRenderTarget(rend, screen_tex);
        SDL_SetRenderDrawColor(rend, 6, 18, 6, 255);
        SDL_RenderClear(rend);

        // draw buffered lines with glow inside render target
        int line_h = FONT_SIZE + LINE_SPACING;
        int scr_inset_x = 18;
        int scr_inset_y = 18;
        int avail_h = (screen_h - 16) - (scr_inset_y * 2) - INPUT_HEIGHT - 16;
        int max_visible = avail_h / line_h;
        if (max_visible < 1) max_visible = 1;
        int start = lb.count - max_visible - scroll;
        if (start < 0) start = 0;
        int yy = scr_inset_y;
        // NOTE: when rendering into render target we re-use render_text_with_glow (it creates textures)
        for (int i = start; i < lb.count && yy + line_h < (screen_h - 16) - scr_inset_y; ++i) {
            render_text_with_glow(rend, font, lb.lines[i], scr_inset_x, yy);
            yy += line_h;
        }

        //Render User Input
        int dynamic_y = yy + 4;

        char promptline[8192];
        const char *disp_path = (strcmp(backend_cwd, ".") == 0) ? "/" : backend_cwd;

        snprintf(promptline, sizeof(promptline), "%s@%s> %s_", current_username, disp_path, inputbuf);

        render_text_with_glow(rend, font, promptline, scr_inset_x, dynamic_y);

        // done drawing to screen_tex
        SDL_SetRenderTarget(rend, NULL);
        // Flicker & jitter - apply to screen_tex when blitting to screen
        double now = SDL_GetTicks() / 1000.0;
        float base = 0.90f + 0.08f * sinf((float)(now * 6.0));
        // occasional random flare/drop
        if ((rand() % 1000) < 6) base -= ((rand() % 40) / 255.0f);
        if ((rand() % 1000) < 4) base += ((rand() % 30) / 255.0f);
        if (base < 0.5f) base = 0.5f;
        if (base > 1.05f) base = 1.05f;
        Uint8 alpha = (Uint8)(base * 255.0f);
        SDL_SetTextureAlphaMod(screen_tex, alpha);
        // slight color modulation (dim / brighten green channel a bit)
        int green_mod = 200 + (int)(55.0f * (base - 0.9f) * 4.0f);
        if (green_mod < 180) green_mod = 180;
        if (green_mod > 255) green_mod = 255;
        SDL_SetTextureColorMod(screen_tex, 180, green_mod, 180);

        // jitter destination position occasionally
        int jitter_x = 0, jitter_y = 0;
        if ((rand() % 100) < 8) {
            jitter_x = (rand() % 3) - 1;
            jitter_y = (rand() % 3) - 1;
        }

        SDL_Rect dst = { screen_x + 8 + jitter_x, screen_y + 8 + jitter_y, screen_w - 16, screen_h - 16 };
        SDL_RenderCopy(rend, screen_tex, NULL, &dst);

        // vignette overlay
        if (vignette) SDL_RenderCopy(rend, vignette, NULL, &dst);

        // scanlines on top (alpha modulated by flicker)
        int scan_alpha = 16 + (int)((1.0f - base) * 80.0f);
        if (scan_alpha < 8) scan_alpha = 8;
        draw_scanlines(rend, dst.x, dst.y, dst.w, dst.h, 3, scan_alpha);

        /* LED positions -- anchored to frame center and clamped to window */
        int led_r = 8;
        int led_spacing = 36;

        /* Use frame center so LEDs move with the frame when window is resized */
        int center_x = screen_x + (screen_w / 2) - (led_spacing / 2);

        /* place just below the frame; keep a small margin and clamp to window height */
        int leds_y = screen_y + screen_h + 24;   /* 12 px below frame (tweak if you want) */
        if (leds_y + led_r + 4 > win_h)          /* clamp if too low */
            leds_y = win_h - led_r - 8;

        /* draw (green on when backend_pid>0, red on when not) */
        draw_led(rend, center_x,              leds_y, led_r,  0, 220, 0,  (backend_pid > 0) ? 1 : 0);
        draw_led(rend, center_x + led_spacing, leds_y, led_r, 220, 0, 0,  (backend_pid <= 0) ? 1 : 0);

         
            int sticker_x = screen_x + screen_w - 180; /* tweak -180 to move further left/right */
            int sticker_y = leds_y - 6;                /* aligns roughly with LEDs baseline */
            render_worn_sticker(rend, font, sticker_text, sticker_x, sticker_y);
        

        SDL_RenderPresent(rend);
        SDL_Delay(10);
    }

    if (screen_tex) SDL_DestroyTexture(screen_tex);
    if (vignette) SDL_DestroyTexture(vignette);
    if (wfd >= 0) close(wfd);
    if (rfd >= 0) close(rfd);
    if (efd >= 0) close(efd);
    if (backend_pid > 0) {
        kill(backend_pid, SIGTERM);
        waitpid(backend_pid, NULL, 0);
    }

    int led_r = 8;
                int led_spacing = 36;
                int center_x = screen_x + (screen_w / 2) - (led_spacing / 2);

                /* put LEDs a bit lower during animation */
                int leds_y = screen_y + screen_h + 24;    /* lowered a little compared to +12 */
                if (leds_y + led_r + 4 > win_h) leds_y = win_h - led_r - 8;

                /* green off (0), red on (1) while loading */
                draw_led(rend, center_x,               leds_y, led_r,  0, 220, 0, 0);
                draw_led(rend, center_x + led_spacing, leds_y, led_r, 220,   0, 0, 1);

             
            int sticker_x = screen_x + screen_w - 180; /* tweak -180 to move further left/right */
            int sticker_y = leds_y - 6;                /* aligns roughly with LEDs baseline */

    lb_free(&lb);
    TTF_CloseFont(font);
    SDL_DestroyRenderer(rend);
    SDL_DestroyWindow(win);
    Mix_Quit();
    Mix_CloseAudio();
    TTF_Quit();
    SDL_Quit();
    return 0;
}