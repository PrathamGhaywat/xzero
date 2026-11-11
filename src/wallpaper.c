#include "wallpaper.h"
#include "util.h"
#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define WP_WIDTH  1280
#define WP_HEIGHT 720

typedef struct {
    int w;
    int h;
    unsigned char *px; // RGB, top-down
} WpImage;

static WpImage *wp_new(int w, int h) {
    WpImage *img = (WpImage*)calloc(1, sizeof(*img));
    if (!img) return NULL;
    img->w = w;
    img->h = h;
    img->px = (unsigned char*)malloc((size_t)w * (size_t)h * 3);
    if (!img->px) {
        free(img);
        return NULL;
    }
    return img;
}

static void wp_free(WpImage *img) {
    if (!img) return;
    free(img->px);
    free(img);
}

static void wp_set(WpImage *img, int x, int y, int r, int g, int b) {
    if (!img || x < 0 || y < 0 || x >= img->w || y >= img->h) return;
    unsigned char *p = img->px + ((size_t)y * (size_t)img->w + (size_t)x) * 3;
    p[0] = (unsigned char)r;
    p[1] = (unsigned char)g;
    p[2] = (unsigned char)b;
}

static void wp_rect(WpImage *img, int x, int y, int w, int h, int r, int g, int b) {
    if (!img) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 > img->w) x1 = img->w;
    if (y1 > img->h) y1 = img->h;
    for (int yy = y0; yy < y1; yy++) {
        for (int xx = x0; xx < x1; xx++) {
            wp_set(img, xx, yy, r, g, b);
        }
    }
}

static void wp_circle(WpImage *img, int cx, int cy, int radius, int r, int g, int b) {
    int rr = radius * radius;
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx*dx + dy*dy <= rr) wp_set(img, cx + dx, cy + dy, r, g, b);
        }
    }
}

static void wp_polygon(WpImage *img, const int *pts, int count, int r, int g, int b) {
    if (!img || !pts || count < 3) return;
    int min_y = pts[1], max_y = pts[1];
    for (int i = 1; i < count; i++) {
        int y = pts[(size_t)i*2 + 1];
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
    }
    double xs[64];
    for (int y = min_y; y <= max_y; y++) {
        int nodes = 0;
        for (int i = 0; i < count; i++) {
            int j = (i + 1) % count;
            int x1 = pts[(size_t)i*2], y1 = pts[(size_t)i*2 + 1];
            int x2 = pts[(size_t)j*2], y2 = pts[(size_t)j*2 + 1];
            if ((y1 <= y && y2 > y) || (y2 <= y && y1 > y)) {
                if (nodes < 64) {
                    xs[nodes++] = (double)x1 + (double)(y - y1) * (double)(x2 - x1) / (double)(y2 - y1);
                }
            }
        }
        for (int i = 0; i < nodes; i++) {
            for (int j = i + 1; j < nodes; j++) {
                if (xs[j] < xs[i]) {
                    double tmp = xs[i];
                    xs[i] = xs[j];
                    xs[j] = tmp;
                }
            }
        }
        for (int i = 0; i + 1 < nodes; i += 2) {
            int xa = (int)(xs[i] + 0.5);
            int xb = (int)(xs[i + 1] + 0.5);
            if (xb > xa) wp_rect(img, xa, y, xb - xa, 1, r, g, b);
        }
    }
}

// 5x7 bitmap font, only the glyphs used below.
static const char *wp_glyph(char c) {
    switch (c) {
    case 'S': return "01111" "10000" "10000" "01110" "00001" "00001" "11110";
    case 'I': return "11111" "00100" "00100" "00100" "00100" "00100" "11111";
    case 'G': return "01110" "10001" "10000" "10111" "10001" "10001" "01110";
    case 'M': return "10001" "11011" "10101" "10101" "10001" "10001" "10001";
    case 'A': return "01110" "10001" "10001" "11111" "10001" "10001" "10001";
    case 'W': return "10001" "10001" "10001" "10101" "10101" "11011" "10001";
    case 'O': return "01110" "10001" "10001" "10001" "10001" "10001" "01110";
    case 'L': return "10000" "10000" "10000" "10000" "10000" "10000" "11111";
    case 'F': return "11111" "10000" "10000" "11110" "10000" "10000" "10000";
    case 'C': return "01110" "10001" "10000" "10000" "10000" "10001" "01110";
    case 'R': return "11110" "10001" "10001" "11110" "10100" "10010" "10001";
    case 'N': return "10001" "11001" "10101" "10011" "10001" "10001" "10001";
    case 'E': return "11111" "10000" "10000" "11110" "10000" "10000" "11111";
    case 'D': return "11110" "10001" "10001" "10001" "10001" "10001" "11110";
    case 'T': return "11111" "00100" "00100" "00100" "00100" "00100" "00100";
    case 'X': return "10001" "10001" "01010" "00100" "01010" "10001" "10001";
    case 'Z': return "11111" "00001" "00010" "00100" "01000" "10000" "11111";
    case ' ': return "00000" "00000" "00000" "00000" "00000" "00000" "00000";
    default:  return NULL;
    }
}

static int wp_text_width(const char *text, int scale) {
    if (!text || !text[0]) return 0;
    return (int)strlen(text) * 6 * scale - scale;
}

static void wp_text(WpImage *img, int x, int y, const char *text, int scale, int r, int g, int b) {
    int cx = x;
    for (const char *p = text; *p; p++) {
        const char *glyph = wp_glyph(*p);
        if (glyph) {
            for (int row = 0; row < 7; row++) {
                for (int col = 0; col < 5; col++) {
                    if (glyph[row*5 + col] == '1') {
                        wp_rect(img, cx + col*scale, y + row*scale, scale, scale, r, g, b);
                    }
                }
            }
        }
        cx += 6 * scale;
    }
}

static void wp_draw_sigma_wolf(WpImage *img) {
    // Night-sky gradient.
    for (int y = 0; y < img->h; y++) {
        int r = 8 + 28 * y / img->h;
        int g = 10 + 4 * y / img->h;
        int b = 28 + 42 * y / img->h;
        wp_rect(img, 0, y, img->w, 1, r, g, b);
    }

    // Stars.
    for (int i = 0; i < 170; i++) {
        int x = (i * 137 + 37) % WP_WIDTH;
        int y = (i * 89 + 13) % 430;
        if (x > 880 && x < 1190 && y > 20 && y < 310) continue;
        int s = (i % 7 == 0) ? 3 : 1;
        wp_rect(img, x, y, s, s, 215, 220, 255);
    }

    // Moon.
    wp_circle(img, 1030, 160, 115, 245, 238, 190);
    wp_circle(img, 1002, 128, 18, 220, 210, 160);
    wp_circle(img, 1062, 198, 14, 220, 210, 160);

    // Wolf head.
    int cx = 640, cy = 300;
    int head[] = {
        cx-60,  cy-190,
        cx-230, cy-320,
        cx-150, cy-100,
        cx-260, cy-30,
        cx-235, cy+100,
        cx-170, cy+170,
        cx-90,  cy+210,
        cx,     cy+230,
        cx+90,  cy+210,
        cx+170, cy+170,
        cx+235, cy+100,
        cx+260, cy-30,
        cx+150, cy-100,
        cx+230, cy-320,
        cx+60,  cy-190
    };
    wp_polygon(img, head, (int)(sizeof(head)/sizeof(head[0])/2), 145, 150, 165);

    int ear_l[] = { cx-195, cy-260, cx-150, cy-100, cx-95, cy-170 };
    int ear_r[] = { cx+195, cy-260, cx+150, cy-100, cx+95,  cy-170 };
    wp_polygon(img, ear_l, 3, 45, 45, 60);
    wp_polygon(img, ear_r, 3, 45, 45, 60);

    // Glowing, extremely serious eyes.
    int eye_l[] = { cx-140, cy-30, cx-45, cy-5, cx-125, cy+20 };
    int eye_r[] = { cx+140, cy-30, cx+45,  cy-5, cx+125, cy+20 };
    wp_polygon(img, eye_l, 3, 255, 70, 30);
    wp_polygon(img, eye_r, 3, 255, 70, 30);

    // Snout, nose, fangs.
    int snout[] = { cx-75, cy+45, cx+75, cy+45, cx+52, cy+155, cx, cy+190, cx-52, cy+155 };
    wp_polygon(img, snout, 5, 175, 180, 195);
    int nose[] = { cx-30, cy+75, cx+30, cy+75, cx, cy+105 };
    wp_polygon(img, nose, 3, 15, 15, 20);
    int fang_l[] = { cx-48, cy+150, cx-26, cy+150, cx-37, cy+205 };
    int fang_r[] = { cx+26, cy+150, cx+48, cy+150, cx+37, cy+205 };
    wp_polygon(img, fang_l, 3, 240, 240, 245);
    wp_polygon(img, fang_r, 3, 240, 240, 245);

    // Cringe typography.
    const char *title = "SIGMA WOLF";
    int title_scale = 12;
    int title_w = wp_text_width(title, title_scale);
    int title_x = (WP_WIDTH - title_w) / 2;
    wp_text(img, title_x + 10, 570, title, title_scale, 0, 0, 0);
    wp_text(img, title_x, 560, title, title_scale, 235, 240, 255);

    const char *sub = "CRINGE EDITION";
    int sub_scale = 5;
    int sub_w = wp_text_width(sub, sub_scale);
    int sub_x = (WP_WIDTH - sub_w) / 2;
    wp_text(img, sub_x + 3, 673, sub, sub_scale, 0, 0, 0);
    wp_text(img, sub_x, 670, sub, sub_scale, 255, 90, 60);

    wp_text(img, 30, 30, "XZERO", 3, 130, 135, 155);
}

static void put_u16(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void put_u32(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static bool wp_write_bmp(const char *path, const WpImage *img, char *err, size_t err_n) {
    size_t row_size = ((size_t)img->w * 3 + 3) & ~(size_t)3;
    size_t data_size = row_size * (size_t)img->h;
    if (data_size > 0xFFFFFFFFu - 54) {
        if (err) snprintf(err, err_n, "image is too large");
        return false;
    }

    unsigned char header[54];
    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    put_u32(header + 2,  (unsigned int)(54 + data_size));
    put_u32(header + 10, 54);
    put_u32(header + 14, 40);
    put_u32(header + 18, (unsigned int)img->w);
    put_u32(header + 22, (unsigned int)img->h);
    put_u16(header + 26, 1);
    put_u16(header + 28, 24);
    put_u32(header + 34, (unsigned int)data_size);
    put_u32(header + 38, 2835);
    put_u32(header + 42, 2835);

    FILE *f = fopen(path, "wb");
    if (!f) {
        if (err) snprintf(err, err_n, "cannot create %s", path);
        return false;
    }

    unsigned char *row = (unsigned char*)calloc(1, row_size);
    if (!row) {
        fclose(f);
        remove(path);
        if (err) snprintf(err, err_n, "out of memory");
        return false;
    }

    bool ok = fwrite(header, 1, sizeof(header), f) == sizeof(header);
    for (int y = img->h - 1; ok && y >= 0; y--) {
        const unsigned char *src = img->px + (size_t)y * (size_t)img->w * 3;
        for (int x = 0; x < img->w; x++) {
            row[x*3 + 0] = src[x*3 + 2];
            row[x*3 + 1] = src[x*3 + 1];
            row[x*3 + 2] = src[x*3 + 0];
        }
        if (fwrite(row, 1, row_size, f) != row_size) ok = false;
    }

    free(row);
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        remove(path);
        if (err) snprintf(err, err_n, "failed to write %s", path);
    }
    return ok;
}

// Prefer a persistent directory on Windows; fall back to the temp dir.
// `persistent` is set when the location survives temp cleanup.
static bool wallpaper_storage_dir(char *dir, size_t n, bool *persistent) {
    if (persistent) *persistent=false;
#ifdef _WIN32
    const char *local_appdata = getenv("LOCALAPPDATA");
    if (local_appdata && local_appdata[0]) {
        util_path_join(dir, n, local_appdata, "xzero/wallpapers");
        if (util_mkdir_p(dir)) {
            if (persistent) *persistent=true;
            return true;
        }
    }
#endif
    util_temp_dir(dir, n);
    return dir[0] != '\0';
}

bool wallpaper_generate_default(char *out_path, size_t out_n, char *err, size_t err_n) {
    if (!out_path || out_n == 0) {
        if (err) snprintf(err, err_n, "invalid output buffer");
        return false;
    }
    char dir[XZERO_PATH_MAX];
    char id[64];
    char fname[XZERO_PATH_MAX];
    char path[XZERO_PATH_MAX];
    bool persistent=false;
    if (!wallpaper_storage_dir(dir, sizeof(dir), &persistent)) {
        if (err) snprintf(err, err_n, "cannot determine wallpaper directory");
        return false;
    }
    if (persistent) {
        util_path_join(path, sizeof(path), dir, "sigma_wolf.bmp");
    } else {
        util_generate_id(id, sizeof(id));
        snprintf(fname, sizeof(fname), "xzero_sigma_wolf_%s.bmp", id);
        util_path_join(path, sizeof(path), dir, fname);
    }

    WpImage *img = wp_new(WP_WIDTH, WP_HEIGHT);
    if (!img) {
        if (err) snprintf(err, err_n, "out of memory");
        return false;
    }
    wp_draw_sigma_wolf(img);
    bool ok = wp_write_bmp(path, img, err, err_n);
    wp_free(img);
    if (!ok) return false;
    util_str_copy(out_path, out_n, path);
    return true;
}

static bool wallpaper_download_url(const char *url, char *out_path, size_t out_n, char *err, size_t err_n) {
    const char *headers[] = { "User-Agent: Mozilla/5.0 (xzero wallpaper)", NULL };
    HttpResponse resp={0};
    int rc = http_get(url, headers, &resp);
    if (rc != 0) {
        if (err) snprintf(err, err_n, "failed to download image");
        http_response_free(&resp);
        return false;
    }
    if (resp.status != 200) {
        if (err) snprintf(err, err_n, "failed to download image (HTTP %ld)", resp.status);
        http_response_free(&resp);
        return false;
    }
    if (!resp.body || resp.body_len == 0) {
        if (err) snprintf(err, err_n, "downloaded image is empty");
        http_response_free(&resp);
        return false;
    }
    if (resp.body_len > 64*1024*1024) {
        if (err) snprintf(err, err_n, "downloaded image is too large (limit 64MB)");
        http_response_free(&resp);
        return false;
    }

    const unsigned char *b = (const unsigned char*)resp.body;
    const char *ext = NULL;
    if (resp.body_len >= 3 && b[0]==0xFF && b[1]==0xD8 && b[2]==0xFF) ext=".jpg";
    else if (resp.body_len >= 4 && b[0]==0x89 && b[1]=='P' && b[2]=='N' && b[3]=='G') ext=".png";
    else if (resp.body_len >= 2 && b[0]=='B' && b[1]=='M') ext=".bmp";
    else if (resp.body_len >= 3 && b[0]=='G' && b[1]=='I' && b[2]=='F') ext=".gif";
    else if (resp.body_len >= 12 && memcmp(b, "RIFF", 4)==0 && memcmp(b+8, "WEBP", 4)==0) ext=".webp";
    if (!ext) {
        if (err) snprintf(err, err_n, "downloaded file is not a supported image (jpg/png/bmp/gif/webp)");
        http_response_free(&resp);
        return false;
    }

    char dir[XZERO_PATH_MAX];
    if (!wallpaper_storage_dir(dir, sizeof(dir), NULL)) {
        if (err) snprintf(err, err_n, "cannot determine wallpaper directory");
        http_response_free(&resp);
        return false;
    }
    char id[64];
    char fname[128];
    char path[XZERO_PATH_MAX];
    util_generate_id(id, sizeof(id));
    snprintf(fname, sizeof(fname), "downloaded_%s%s", id, ext);
    util_path_join(path, sizeof(path), dir, fname);

    FILE *f = fopen(path, "wb");
    if (!f) {
        if (err) snprintf(err, err_n, "cannot create %s", path);
        http_response_free(&resp);
        return false;
    }
    size_t written = fwrite(resp.body, 1, resp.body_len, f);
    bool ok = (fclose(f) == 0) && written == resp.body_len;
    http_response_free(&resp);
    if (!ok) {
        remove(path);
        if (err) snprintf(err, err_n, "failed to save downloaded image");
        return false;
    }
    util_str_copy(out_path, out_n, path);
    return true;
}

int wallpaper_set(const char *path, char *err, size_t err_n) {
    char resolved[XZERO_PATH_MAX];
    const char *image = path;
    bool prepared = false;
    if (!image || !image[0]) {
        if (!wallpaper_generate_default(resolved, sizeof(resolved), err, err_n)) return -1;
        image = resolved;
        prepared = true;
    } else if (strncmp(image, "http://", 7) == 0 || strncmp(image, "https://", 8) == 0) {
        if (!wallpaper_download_url(image, resolved, sizeof(resolved), err, err_n)) return -1;
        image = resolved;
        prepared = true;
    }

#ifdef _WIN32
    WCHAR wide[XZERO_PATH_MAX * 2];
    int n = MultiByteToWideChar(CP_UTF8, 0, image, -1, wide, (int)(sizeof(wide)/sizeof(wide[0])));
    if (n <= 0) {
        if (err) snprintf(err, err_n, "invalid image path encoding");
        return -1;
    }
    DWORD attr = GetFileAttributesW(wide);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        if (err) snprintf(err, err_n, "image not found: %s", image);
        return -1;
    }

    WCHAR absolute[XZERO_PATH_MAX * 2];
    const DWORD absolute_cap = (DWORD)(sizeof(absolute)/sizeof(absolute[0]));
    DWORD full = GetFullPathNameW(wide, absolute_cap, absolute, NULL);
    if (full == 0 || full >= absolute_cap) {
        lstrcpynW(absolute, wide, (int)absolute_cap);
    }

    if (!SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, absolute,
                               SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)) {
        if (err) snprintf(err, err_n, "SystemParametersInfo failed (error %lu)",
                          (unsigned long)GetLastError());
        return -1;
    }
    return 0;
#else
    if (prepared) {
        if (err) snprintf(err, err_n, "wallpaper is only supported on Windows (image saved at %s)", image);
    } else if (err) {
        snprintf(err, err_n, "wallpaper is only supported on Windows");
    }
    return -1;
#endif
}
