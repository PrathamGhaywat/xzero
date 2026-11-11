#include "registry.h"
#include "../util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#endif

#define BASH_DEFAULT_TIMEOUT_MS 120000
#define BASH_MAX_TIMEOUT_MS     3600000
#define BASH_TAIL_BYTES         ((size_t)(50*1024 + 4096))
#define BASH_MAX_SPILL_BYTES    ((size_t)(64*1024*1024))

// Collects command output while keeping memory bounded: the full output goes
// to the spill file, and only the last ~50KB is retained for the model.
typedef struct {
    FILE *spill;
    bool spill_ok;
    size_t stored_bytes;
    size_t total_bytes;
    size_t newlines;
    char *tail;
    size_t tail_len;
    size_t tail_cap;
} OutputCollector;

static void collector_init(OutputCollector *c, const char *spill_path) {
    memset(c, 0, sizeof(*c));
    c->tail_cap = BASH_TAIL_BYTES;
    c->tail = (char*)malloc(c->tail_cap + 1);
    if(c->tail) c->tail[0]='\0';
    c->spill = fopen(spill_path, "wb");
    c->spill_ok = (c->spill != NULL);
}

#if defined(_MSC_VER)
#define XZERO_NOINLINE __declspec(noinline)
#elif defined(__GNUC__)
#define XZERO_NOINLINE __attribute__((noinline))
#else
#define XZERO_NOINLINE
#endif

static XZERO_NOINLINE void collector_write(OutputCollector *c, const char *data, size_t len) {
    if(!c || !data || len==0) return;
    c->total_bytes += len;
    for(size_t i=0;i<len;i++) if(data[i]=='\n') c->newlines++;

    if(c->spill && c->stored_bytes < BASH_MAX_SPILL_BYTES){
        size_t remaining = BASH_MAX_SPILL_BYTES - c->stored_bytes;
        size_t to_write = len < remaining ? len : remaining;
        if(fwrite(data, 1, to_write, c->spill)!=to_write){
            fclose(c->spill);
            c->spill=NULL;
            c->spill_ok=false;
        } else {
            c->stored_bytes += to_write;
        }
    }

    if(!c->tail || c->tail_cap==0) return;
    if(len >= c->tail_cap){
        memcpy(c->tail, data + len - c->tail_cap, c->tail_cap);
        c->tail_len = c->tail_cap;
    } else if(c->tail_len + len <= c->tail_cap){
        memcpy(c->tail + c->tail_len, data, len);
        c->tail_len += len;
    } else {
        size_t drop = c->tail_len + len - c->tail_cap;
        memmove(c->tail, c->tail + drop, c->tail_len - drop);
        memcpy(c->tail + c->tail_len - drop, data, len);
        c->tail_len = c->tail_cap;
    }
    c->tail[c->tail_len]='\0';
}

static void collector_close(OutputCollector *c) {
    if(c->spill){
        fflush(c->spill);
        fclose(c->spill);
        c->spill=NULL;
    }
}

static void collector_free(OutputCollector *c) {
    collector_close(c);
    free(c->tail);
    c->tail=NULL;
    c->tail_len=0;
}

#ifndef _WIN32
static long long now_ms(void) {
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC, &ts)!=0) return 0;
    return (long long)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}

// Run through /bin/sh with stdout+stderr merged. The child gets its own
// process group so a timeout can terminate the whole tree.
static int run_command(const char *command, int timeout_ms, OutputCollector *out, bool *timed_out) {
    *timed_out=false;
    int fds[2];
    if(pipe(fds)!=0) return -1;
    pid_t pid=fork();
    if(pid<0){
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if(pid==0){
        setpgid(0,0);
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", command, (char*)NULL);
        _exit(127);
    }
    close(fds[1]);
    setpgid(pid, pid);

    long long deadline = now_ms() + timeout_ms;
    bool killed=false;
    char buf[8192];
    for(;;){
        long long remaining = deadline - now_ms();
        if(remaining <= 0){ killed=true; break; }
        struct pollfd pfd;
        pfd.fd=fds[0];
        pfd.events=POLLIN;
        pfd.revents=0;
        int pr = poll(&pfd, 1, (int)(remaining > 1000 ? 1000 : remaining));
        if(pr<0){
            if(errno==EINTR) continue;
            break;
        }
        if(pr==0) continue;
        ssize_t n = read(fds[0], buf, sizeof(buf));
        if(n>0){ collector_write(out, buf, (size_t)n); continue; }
        if(n==0) break;
        if(errno==EINTR) continue;
        break;
    }
    if(killed){
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
    }
    close(fds[0]);

    int status=0;
    while(waitpid(pid, &status, 0)<0){
        if(errno!=EINTR) break;
    }
    *timed_out=killed;
    if(killed) return -1;
    if(WIFEXITED(status)) return WEXITSTATUS(status);
    if(WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}
#else
// Windows equivalent: cmd.exe with both output handles pointing at one pipe.
static int run_command(const char *command, int timeout_ms, OutputCollector *out, bool *timed_out) {
    *timed_out=false;
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength=sizeof(sa);
    sa.bInheritHandle=TRUE;

    HANDLE rd=NULL, wr=NULL;
    if(!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb=sizeof(si);
    si.dwFlags=STARTF_USESTDHANDLES;
    si.hStdOutput=wr;
    si.hStdError=wr;
    si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    size_t cmd_len = strlen(command) + 32;
    char *cmdline = (char*)malloc(cmd_len);
    if(!cmdline){
        CloseHandle(rd);
        CloseHandle(wr);
        return -1;
    }
    snprintf(cmdline, cmd_len, "cmd.exe /C %s", command);
    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(cmdline);
    CloseHandle(wr);
    if(!ok){
        CloseHandle(rd);
        return -1;
    }
    CloseHandle(pi.hThread);

    ULONGLONG start = GetTickCount64();
    bool killed=false;
    char buf[8192];
    for(;;){
        if(GetTickCount64() - start >= (ULONGLONG)timeout_ms){
            killed=true;
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
        DWORD avail=0;
        if(!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL)) break;
        if(avail>0){
            DWORD n=0;
            if(!ReadFile(rd, buf, sizeof(buf), &n, NULL) || n==0) break;
            collector_write(out, buf, n);
            continue;
        }
        DWORD w = WaitForSingleObject(pi.hProcess, 50);
        if(w==WAIT_OBJECT_0){
            // Drain bytes that arrived just before the process exited.
            while(PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail>0){
                DWORD n=0;
                if(!ReadFile(rd, buf, sizeof(buf), &n, NULL) || n==0) break;
                collector_write(out, buf, n);
            }
            break;
        }
    }

    DWORD code=0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(rd);
    CloseHandle(pi.hProcess);
    *timed_out=killed;
    if(killed) return -1;
    return (int)code;
}
#endif

ToolResult tool_bash(cJSON *args, Session *session) {
    (void)session;
    ToolResult r={0};
    const char *command=NULL;
    int timeout_ms = BASH_DEFAULT_TIMEOUT_MS;
    if(args){
        cJSON *c=cJSON_GetObjectItem(args,"command");
        if(cJSON_IsString(c)) command=c->valuestring;
        cJSON *t=cJSON_GetObjectItem(args,"timeout");
        if(cJSON_IsNumber(t)) timeout_ms=t->valueint;
        // also support timeout_ms
        cJSON *tm=cJSON_GetObjectItem(args,"timeout_ms");
        if(cJSON_IsNumber(tm)) timeout_ms=tm->valueint;
    }
    if(!command||!command[0]){ r.content=strdup("error: missing 'command'"); r.is_error=true; return r; }
    if(timeout_ms <= 0) timeout_ms = BASH_DEFAULT_TIMEOUT_MS;
    if(timeout_ms > BASH_MAX_TIMEOUT_MS) timeout_ms = BASH_MAX_TIMEOUT_MS;

    const char *cmd_to_run = command;
#ifdef _WIN32
    // Windows compatibility: translate common POSIX commands
    char translated[2048];
    while(*cmd_to_run==' ' || *cmd_to_run=='\t') cmd_to_run++;
    if(strncmp(cmd_to_run, "pwd", 3)==0 && (cmd_to_run[3]=='\0' || cmd_to_run[3]==' ' || cmd_to_run[3]=='\t' || cmd_to_run[3]=='\n')){
        cmd_to_run = "cd";
    } else if(strncmp(cmd_to_run, "ls", 2)==0 && (cmd_to_run[2]=='\0' || cmd_to_run[2]==' ')){
        if(cmd_to_run[2]=='\0') cmd_to_run = "dir /b";
        else {
            const char *rest = cmd_to_run+2;
            while(*rest==' ' || *rest=='\t') rest++;
            while(*rest=='-'){
                while(*rest && *rest!=' ' && *rest!='\t') rest++;
                while(*rest==' ' || *rest=='\t') rest++;
            }
            if(*rest=='\0') snprintf(translated, sizeof(translated), "dir /b");
            else snprintf(translated, sizeof(translated), "dir /b \"%s\"", rest);
            cmd_to_run = translated;
        }
    } else if(strncmp(cmd_to_run, "cat ", 4)==0 || strncmp(cmd_to_run, "cat\t", 4)==0){
        snprintf(translated, sizeof(translated), "type %s", cmd_to_run+4);
        cmd_to_run = translated;
    }
#endif

    if(strcmp(cmd_to_run, command)!=0){
        printf("[xzero] bash: `%s` -> `%s`\n", command, cmd_to_run);
    } else {
        printf("[xzero] bash: `%s`\n", command);
    }
    fflush(stdout);

    char tmpdir[XZERO_PATH_MAX];
    char spill[XZERO_PATH_MAX];
    char id[64];
    char fname[XZERO_PATH_MAX];
    util_temp_dir(tmpdir, sizeof(tmpdir));
    util_generate_id(id, sizeof(id));
    snprintf(fname, sizeof(fname), "xzero_bash_%s.txt", id);
    util_path_join(spill, sizeof(spill), tmpdir, fname);

    OutputCollector col;
    collector_init(&col, spill);
    bool timed_out=false;
    int rc = run_command(cmd_to_run, timeout_ms, &col, &timed_out);
    collector_close(&col);

    if(rc==-1 && !timed_out){
        collector_free(&col);
        remove(spill);
        r.content=strdup("error: failed to execute command");
        r.is_error=true;
        return r;
    }

    size_t total_lines = col.newlines + (col.total_bytes ? 1 : 0);
    bool trunc=false;
    size_t lines=0;
    char *capped = util_cap_output_total(col.tail, col.tail_len, col.total_bytes, total_lines,
                                         spill, &trunc, &lines);
    collector_free(&col);
    if(!capped){
        remove(spill);
        r.content=strdup("error: oom");
        r.is_error=true;
        return r;
    }

    char note[256];
    note[0]='\0';
    if(timed_out){
        snprintf(note, sizeof(note), "\n[timed out after %d ms]", timeout_ms);
    } else if(rc!=0){
        snprintf(note, sizeof(note), "\n[exit code %d]", rc);
    }
    if(note[0]){
        size_t cl=strlen(capped);
        size_t nl=strlen(note);
        char *with_note=(char*)malloc(cl+nl+1);
        if(with_note){
            memcpy(with_note, capped, cl);
            memcpy(with_note+cl, note, nl+1);
            free(capped);
            capped=with_note;
        }
    }

    if(timed_out){
        printf("%s\n[timed out after %d ms]\n", capped, timeout_ms);
    } else {
        printf("%s\n[exit %d]\n", capped, rc);
    }
    fflush(stdout);

    r.content=capped;
    r.truncated=trunc;
    if(trunc){
        if(col.spill_ok) r.full_path=strdup(spill);
    } else {
        remove(spill);
    }
    return r;
}
