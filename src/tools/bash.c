#include "registry.h"
#include "../util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

ToolResult tool_bash(cJSON *args, Session *session) {
    (void)session;
    ToolResult r={0};
    const char *command=NULL;
    int timeout=120000;
    if(args){
        cJSON *c=cJSON_GetObjectItem(args,"command");
        if(cJSON_IsString(c)) command=c->valuestring;
        cJSON *t=cJSON_GetObjectItem(args,"timeout");
        if(cJSON_IsNumber(t)) timeout=t->valueint;
        // also support timeout_ms
        cJSON *tm=cJSON_GetObjectItem(args,"timeout_ms");
        if(cJSON_IsNumber(tm)) timeout=tm->valueint;
    }
    if(!command||!command[0]){ r.content=strdup("error: missing 'command'"); r.is_error=true; return r; }

    // Windows compatibility: translate common POSIX commands
#ifdef _WIN32
    char translated[2048];
    const char *cmd_to_run = command;
    // Trim leading spaces
    while(*cmd_to_run==' ' || *cmd_to_run=='\t') cmd_to_run++;
    if(strncmp(cmd_to_run, "pwd", 3)==0 && (cmd_to_run[3]=='\0' || cmd_to_run[3]==' ' || cmd_to_run[3]=='\t' || cmd_to_run[3]=='\n')){
        // pwd -> cd (no args)
        if(cmd_to_run[3]=='\0') cmd_to_run = "cd";
        else {
            // pwd with args like "pwd -P" -> just cd
            cmd_to_run = "cd";
        }
    } else if(strncmp(cmd_to_run, "ls", 2)==0 && (cmd_to_run[2]=='\0' || cmd_to_run[2]==' ')){
        // ls -> dir
        if(cmd_to_run[2]=='\0') cmd_to_run = "dir /b";
        else {
            // naive: ls -la / ls <path> -> dir <path>
            const char *rest = cmd_to_run+2;
            while(*rest==' ' || *rest=='\t') rest++;
            // skip flags like -la, -l, -a
            while(*rest=='-'){
                while(*rest && *rest!=' ' && *rest!='\t') rest++;
                while(*rest==' ' || *rest=='\t') rest++;
            }
            if(*rest=='\0') snprintf(translated, sizeof(translated), "dir /b");
            else snprintf(translated, sizeof(translated), "dir /b \"%s\"", rest);
            cmd_to_run = translated;
        }
    } else if(strncmp(cmd_to_run, "cat ", 4)==0){
        snprintf(translated, sizeof(translated), "type %s", cmd_to_run+4);
        cmd_to_run = translated;
    } else if(strncmp(cmd_to_run, "cat\t", 4)==0){
        snprintf(translated, sizeof(translated), "type %s", cmd_to_run+4);
        cmd_to_run = translated;
    }
    // Build command with stderr merged
    char cmd_with_stderr[4096];
    snprintf(cmd_with_stderr, sizeof(cmd_with_stderr), "%s 2>&1", cmd_to_run);
    // Show exact command to human (original + translated if different)
    if(strcmp(cmd_to_run, command)!=0){
        printf("[xzero] bash: `%s` -> `%s`\n", command, cmd_to_run);
    } else {
        printf("[xzero] bash: `%s`\n", command);
    }
    fflush(stdout);
    cmd_to_run = cmd_with_stderr;
#endif

    // Leverage: cap output 50KB/2000 lines, return tail + spill (Lever #1)
    // Cross-platform popen
#ifdef _WIN32
    FILE *pipe = _popen(cmd_to_run, "rb");
#else
    // On POSIX, also merge stderr
    char posix_cmd[4096];
    snprintf(posix_cmd, sizeof(posix_cmd), "%s 2>&1", command);
    printf("[xzero] bash: `%s`\n", command);
    fflush(stdout);
    FILE *pipe = popen(posix_cmd, "r");
#endif
    if(!pipe){ r.content=strdup("error: popen failed"); r.is_error=true; return r; }

    // Read all output (may be large - read bounded but need full for spill)
    size_t cap=8192;
    size_t len=0;
    char *buf=(char*)malloc(cap);
    if(!buf){
#ifdef _WIN32
        _pclose(pipe);
#else
        pclose(pipe);
#endif
        r.content=strdup("error: oom"); r.is_error=true; return r;
    }
    char tmp[4096];
    size_t n;
    while((n=fread(tmp,1,sizeof(tmp),pipe))>0){
        if(len+n+1 > cap){
            size_t newcap=cap*2;
            while(newcap < len+n+1) newcap*=2;
            char *nb=(char*)realloc(buf,newcap);
            if(!nb){ free(buf);
#ifdef _WIN32
                _pclose(pipe);
#else
                pclose(pipe);
#endif
                r.content=strdup("error: oom"); r.is_error=true; return r;
            }
            buf=nb; cap=newcap;
        }
        memcpy(buf+len, tmp, n);
        len+=n;
    }
#ifdef _WIN32
    int rc=_pclose(pipe);
#else
    int rc=pclose(pipe);
#endif
    buf[len]='\0';

    // Build result with exit code
    // Human gets full output (print to stdout if not too large, else spill path)
    // Model gets capped tail
    char spill[XZERO_PATH_MAX];
    util_temp_dir(spill,sizeof(spill));
    char id[64];
    util_generate_id(id,sizeof(id));
    char fname[XZERO_PATH_MAX];
    snprintf(fname,sizeof(fname),"xzero_bash_%s.txt", id);
    util_path_join(spill,sizeof(spill), spill, fname);

    // For model: cap
    bool trunc=false;
    size_t lines=0;
    char *capped = util_cap_output(buf, len, spill, &trunc, &lines);
    // For human: print capped already? But show full hint
    // We'll spill full if truncated
    if(trunc){
        FILE *sf=fopen(spill,"wb");
        if(sf){ fwrite(buf,1,len,sf); fclose(sf); }
        // Human sees full via file path hint already in capped footer
        // Also print to stdout the capped tail for visibility
        printf("%s\n[exit %d, truncated %zu lines -> %s]\n", capped, rc, lines, spill);
    } else {
        // Human and model same
        printf("%s\n[exit %d]\n", buf, rc);
    }
    fflush(stdout);

    free(buf);
    r.content=capped;
    r.truncated=trunc;
    if(trunc) r.full_path=strdup(spill);
    // is_error if rc !=0 ? but still return content; mark if needed
    if(rc!=0){
        // append exit code to content for model
        size_t cl=strlen(r.content);
        char *with_code=(char*)malloc(cl+64);
        if(with_code){
            memcpy(with_code, r.content, cl);
            snprintf(with_code+cl, 64, "\n[exit code %d]", rc);
            free(r.content);
            r.content=with_code;
        }
    }
    return r;
}
