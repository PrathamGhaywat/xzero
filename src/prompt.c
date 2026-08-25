#include "prompt.h"
#include "util.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#include <termios.h>
#endif

bool prompt_input(const char *prompt, const char *def, char *out, size_t n) {
    if (!out || n==0) return false;
    out[0]='\0';
    if (prompt) {
        if (def && def[0]) printf("%s [%s]: ", prompt, def);
        else printf("%s: ", prompt);
        fflush(stdout);
    }
    char buf[2048];
    if (!fgets(buf, sizeof(buf), stdin)) return false;
    // strip newline
    size_t len = strlen(buf);
    while (len>0 && (buf[len-1]=='\n' || buf[len-1]=='\r')) buf[--len]='\0';
    util_trim(buf);
    if (buf[0]=='\0' && def) util_str_copy(out, n, def);
    else util_str_copy(out, n, buf);
    return true;
}

bool prompt_password(const char *prompt, char *out, size_t n) {
    if (!out || n==0) return false;
    out[0]='\0';
    if (prompt) { printf("%s", prompt); fflush(stdout); }

#ifdef _WIN32
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode=0;
    bool is_tty = false;
    if (hIn != INVALID_HANDLE_VALUE && GetConsoleMode(hIn, &mode)) is_tty=true;

    if (is_tty) {
        DWORD orig = mode;
        SetConsoleMode(hIn, mode & ~ENABLE_ECHO_INPUT);
        char buf[2048];
        if (!fgets(buf, sizeof(buf), stdin)) {
            SetConsoleMode(hIn, orig);
            printf("\n");
            return false;
        }
        SetConsoleMode(hIn, orig);
        printf("\n");
        size_t len=strlen(buf);
        while(len>0 && (buf[len-1]=='\n' || buf[len-1]=='\r')) buf[--len]='\0';
        util_str_copy(out, n, buf);
        // clear buf
        memset(buf, 0, sizeof(buf));
        return true;
    } else {
        // not tty
        char buf[2048];
        if (!fgets(buf, sizeof(buf), stdin)) return false;
        size_t len=strlen(buf);
        while(len>0 && (buf[len-1]=='\n' || buf[len-1]=='\r')) buf[--len]='\0';
        util_str_copy(out, n, buf);
        return true;
    }
#else
    bool is_tty = isatty(fileno(stdin));
    if (is_tty) {
        struct termios old, newt;
        if (tcgetattr(STDIN_FILENO, &old)==0) {
            newt=old;
            newt.c_lflag &= ~ECHO;
            tcsetattr(STDIN_FILENO, TCSANOW, &newt);
            char buf[2048];
            bool ok=false;
            if (fgets(buf, sizeof(buf), stdin)) {
                size_t len=strlen(buf);
                while(len>0 && (buf[len-1]=='\n' || buf[len-1]=='\r')) buf[--len]='\0';
                util_str_copy(out, n, buf);
                memset(buf,0,sizeof(buf));
                ok=true;
            }
            tcsetattr(STDIN_FILENO, TCSANOW, &old);
            printf("\n");
            return ok;
        }
    }
    char buf[2048];
    if (!fgets(buf, sizeof(buf), stdin)) return false;
    size_t len=strlen(buf);
    while(len>0 && (buf[len-1]=='\n' || buf[len-1]=='\r')) buf[--len]='\0';
    util_str_copy(out, n, buf);
    return true;
#endif
}

bool prompt_confirm(const char *prompt, bool def_yes) {
    printf("%s [%s]: ", prompt, def_yes ? "Y/n" : "y/N");
    fflush(stdout);
    char buf[32];
    if (!fgets(buf, sizeof(buf), stdin)) return def_yes;
    util_trim(buf);
    if (buf[0]=='\0') return def_yes;
    if (buf[0]=='y' || buf[0]=='Y') return true;
    if (buf[0]=='n' || buf[0]=='N') return false;
    return def_yes;
}
