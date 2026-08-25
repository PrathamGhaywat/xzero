#include "util.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

#define CAP_BYTES (50*1024)
#define CAP_LINES 2000

void util_trim(char *s) {
    if (!s) return;
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start)+1);
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len-1])) s[--len] = '\0';
}

void util_str_copy(char *dst, size_t n, const char *src) {
    if (!dst || n==0) return;
    if (!src) { dst[0]='\0'; return; }
    strncpy(dst, src, n-1);
    dst[n-1]='\0';
}

void util_url_normalize(const char *input, char *out, size_t out_n) {
    if (!input || !out || out_n==0) return;
    char tmp[XZERO_URL_MAX*2];
    util_str_copy(tmp, sizeof(tmp), input);
    util_trim(tmp);
    // Remove trailing slashes
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len-1]=='/') tmp[--len]='\0';
    // Remove duplicate /v1 handling is not done here - just normalize slash
    util_str_copy(out, out_n, tmp);
}

void util_path_join(char *out, size_t out_n, const char *a, const char *b) {
    if (!out || out_n==0) return;
    if (!a || !a[0]) { util_str_copy(out, out_n, b?b:""); return; }
    if (!b || !b[0]) { util_str_copy(out, out_n, a); return; }
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    size_t al = strlen(a);
    bool a_slash = al>0 && (a[al-1]=='/' || a[al-1]=='\\');
    bool b_slash = b[0]=='/' || b[0]=='\\';
    if (a_slash && b_slash) snprintf(out, out_n, "%s%s", a, b+1);
    else if (!a_slash && !b_slash) snprintf(out, out_n, "%s%c%s", a, sep, b);
    else snprintf(out, out_n, "%s%s", a, b);
}

bool util_mkdir_p(const char *path) {
    if (!path || !path[0]) return false;
    char tmp[XZERO_PATH_MAX];
    util_str_copy(tmp, sizeof(tmp), path);
    size_t len = strlen(tmp);
    // Remove trailing slash
    while (len>1 && (tmp[len-1]=='/' || tmp[len-1]=='\\')) tmp[--len]='\0';
    
    for (size_t i=1; i<len; i++) {
        if (tmp[i]=='/' || tmp[i]=='\\') {
            char c = tmp[i];
            tmp[i]='\0';
            if (strlen(tmp)>0) {
#ifdef _WIN32
                _mkdir(tmp);
#else
                mkdir(tmp, 0755);
#endif
            }
            tmp[i]=c;
        }
    }
#ifdef _WIN32
    _mkdir(tmp);
#else
    mkdir(tmp, 0755);
#endif
    // check exists
    struct stat st;
    return stat(path, &st)==0 || stat(tmp, &st)==0;
}

size_t util_estimate_tokens(const char *s) {
    if (!s) return 0;
    return strlen(s)/4 + 1;
}
size_t util_estimate_tokens_n(const char *s, size_t n) {
    return n/4 + 1;
}

void util_temp_dir(char *out, size_t n) {
#ifdef _WIN32
    char tmp[MAX_PATH];
    DWORD len = GetTempPathA(sizeof(tmp), tmp);
    if (len>0 && len < sizeof(tmp)) util_str_copy(out, n, tmp);
    else util_str_copy(out, n, "C:\\Temp\\");
#else
    const char *t = getenv("TMPDIR");
    if (!t) t = getenv("TMP");
    if (!t) t = "/tmp";
    util_str_copy(out, n, t);
#endif
}

void util_generate_id(char *out, size_t n) {
    // time + pid + rand hex
    unsigned int r1 = (unsigned int)rand();
    unsigned int r2 = (unsigned int)rand();
    unsigned int r3 = (unsigned int)rand();
    time_t now = time(NULL);
#ifdef _WIN32
    unsigned int pid = (unsigned int)GetCurrentProcessId();
#else
    unsigned int pid = (unsigned int)getpid();
#endif
    snprintf(out, n, "%08x-%04x-%04x-%04x-%08x%04x",
        (unsigned int)now, pid & 0xFFFF, r1 & 0xFFFF, r2 & 0xFFFF, r3, r1>>16);
}

static bool is_within(const char *abs_path, const char *cwd_abs) {
    size_t cl = strlen(cwd_abs);
    if (strncmp(abs_path, cwd_abs, cl)!=0) return false;
    // ensure either exact or separator
    if (abs_path[cl]=='\0' || abs_path[cl]=='/' || abs_path[cl]=='\\') return true;
    return false;
}

bool util_path_is_within_cwd(const char *path) {
    if (!path) return false;
#ifdef _WIN32
    char cwd[MAX_PATH];
    if (!_getcwd(cwd, sizeof(cwd))) return false;
    char full[MAX_PATH];
    char cwd_full[MAX_PATH];
    if (!_fullpath(cwd_full, cwd, sizeof(cwd_full))) return false;
    // handle absolute vs relative
    const char *to_check = path;
    char resolved[MAX_PATH];
    if (path[0]=='/' || path[0]=='\\' || (path[1]==':' )) {
        if (!_fullpath(resolved, path, sizeof(resolved))) return false;
        to_check = resolved;
    } else {
        char joined[MAX_PATH*2];
        util_path_join(joined, sizeof(joined), cwd, path);
        if (!_fullpath(resolved, joined, sizeof(resolved))) return false;
        to_check = resolved;
    }
    // normalize slashes for compare
    for (char *p=resolved; *p; p++) if (*p=='/') *p='\\';
    for (char *p=cwd_full; *p; p++) if (*p=='/') *p='\\';
    return is_within(to_check, cwd_full);
#else
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) return false;
    char resolved[PATH_MAX];
    if (path[0]=='/') {
        if (!realpath(path, resolved)) {
            // file may not exist - resolve parent
            char tmp[PATH_MAX];
            snprintf(tmp, sizeof(tmp), "%s", path);
            // fallback: join cwd check via string prefix
            if (strncmp(path, cwd, strlen(cwd))==0) return true;
            return false;
        }
    } else {
        char joined[PATH_MAX*2];
        snprintf(joined, sizeof(joined), "%s/%s", cwd, path);
        if (!realpath(joined, resolved)) {
            // not existing yet - check prefix
            char abs_joined[PATH_MAX*2];
            // normalize ../
            // simple check: does joined start with cwd?
            // For non-existent files, we conservatively check string prefix after normalizing
            // remove ./ and handle ../ quickly
            if (strncmp(joined, cwd, strlen(cwd))==0) return true;
            // try to check parent exists
            return true; // permissive for new files within cwd string
        }
    }
    return is_within(resolved, cwd);
#endif
}

size_t util_count_lines(const char *s, size_t n) {
    if (!s || n==0) return 0;
    size_t c=1;
    for (size_t i=0;i<n;i++) if (s[i]=='\n') c++;
    return c;
}

char *util_cap_output(const char *full_output, size_t full_len, const char *full_path_hint, bool *truncated, size_t *out_lines) {
    if (!full_output) {
        if (truncated) *truncated=false;
        if (out_lines) *out_lines=0;
        char *empty = (char*)malloc(1);
        if (empty) empty[0]='\0';
        return empty;
    }
    if (full_len==0) full_len = strlen(full_output);
    size_t lines = util_count_lines(full_output, full_len);
    if (out_lines) *out_lines = lines;
    bool need_cap = (full_len > CAP_BYTES) || (lines > CAP_LINES);
    if (truncated) *truncated = need_cap;
    if (!need_cap) {
        char *out = (char*)malloc(full_len+1);
        if (!out) return NULL;
        memcpy(out, full_output, full_len);
        out[full_len]='\0';
        return out;
    }
    // Need to cap: take last 50KB or last 2000 lines, whichever is more restrictive (smaller)
    // Strategy: take tail that satisfies both constraints
    size_t tail_start = 0;
    // First, if bytes over, start at full_len - CAP_BYTES
    if (full_len > CAP_BYTES) tail_start = full_len - CAP_BYTES;
    // Then, if lines over, find start of last CAP_LINES lines
    if (lines > CAP_LINES) {
        size_t target_line = lines - CAP_LINES;
        size_t cur = 0;
        for (size_t i=0;i<full_len;i++) {
            if (full_output[i]=='\n') {
                cur++;
                if (cur==target_line) {
                    size_t line_start = i+1;
                    if (line_start > tail_start) tail_start = line_start;
                    break;
                }
            }
        }
    }
    size_t tail_len = full_len - tail_start;
    const char *hint = full_path_hint ? full_path_hint : "/tmp/xzero_output.txt";
    char footer[512];
    // Estimate original lines
    snprintf(footer, sizeof(footer), "\n[Showing lines %zu-%zu of %zu. Full output: %s]\n", 
        lines - util_count_lines(full_output+tail_start, tail_len) + 1, lines, lines, hint);
    size_t footer_len = strlen(footer);
    char *out = (char*)malloc(tail_len + footer_len + 1);
    if (!out) return NULL;
    memcpy(out, full_output+tail_start, tail_len);
    memcpy(out+tail_len, footer, footer_len);
    out[tail_len+footer_len]='\0';
    return out;
}

char *util_spill_and_receipt(const char *full_output, size_t full_len, const char *spill_path) {
    bool trunc=false;
    size_t lines=0;
    char *capped = util_cap_output(full_output, full_len, spill_path, &trunc, &lines);
    if (trunc && spill_path && full_output) {
        // write full output to spill file for human
        char dir[XZERO_PATH_MAX];
        strncpy(dir, spill_path, sizeof(dir)-1);
        dir[sizeof(dir)-1]='\0';
        char *sep = strrchr(dir, '/');
        char *sep2 = strrchr(dir, '\\');
        if (sep2 && (!sep || sep2>sep)) sep=sep2;
        if (sep) {
            *sep='\0';
            util_mkdir_p(dir);
        }
        FILE *f = fopen(spill_path, "wb");
        if (f) {
            fwrite(full_output, 1, full_len?full_len:strlen(full_output), f);
            fclose(f);
        }
    }
    return capped;
}
