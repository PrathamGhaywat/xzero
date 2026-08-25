#include "registry.h"
#include "../util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <fnmatch.h>
#endif

#define GLOB_MAX 200
#define GLOB_CAP  (50*1024)

#ifdef _WIN32
static void glob_win_rec(const char *base, const char *pattern, char *out, size_t *len, size_t cap, int *count, bool *stop){
    if(*stop) return;
    char search[XZERO_PATH_MAX];
    util_path_join(search,sizeof(search), base, "*");
    WIN32_FIND_DATAA fd;
    HANDLE h=FindFirstFileA(search,&fd);
    if(h==INVALID_HANDLE_VALUE) return;
    do{
        if(strcmp(fd.cFileName,".")==0||strcmp(fd.cFileName,"..")==0) continue;
        if(*stop) break;
        char full[XZERO_PATH_MAX];
        util_path_join(full,sizeof(full), base, fd.cFileName);
        // check match for file
        bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)!=0;
        // simple pattern: use PathMatchSpec if available, else substring
        bool match=false;
        if(strchr(pattern,'*')||strchr(pattern,'?')){
            // simplistic: pattern like "**/*.c" -> check suffix
            const char *suffix=strrchr(pattern,'.');
            if(suffix && strstr(full,suffix)) match=true;
            else if(strstr(full, pattern)) match=true;
            // For glob we do naive: if pattern contains "*", check extension
            // Use custom: convert pattern to match via wildcard
            // For now try using fnmatch-like via checking extension
            // Better: use Windows PathMatchSpecA
            // Try load
            // Fallback already done
        } else {
            if(strstr(full, pattern)) match=true;
        }
        // More robust: try to use shlwapi PathMatchSpec if linked? We'll just do substring for now
        // But for pattern "*.c" we match extension
        // Let's do simple wildcard: "*" matches all
        if(strcmp(pattern,"*")==0) match=true;
        else if(pattern[0]=='*' && pattern[1]=='.'){
            const char *ext=strrchr(full,'.');
            if(ext && strcmp(ext, pattern+1)==0) match=true;
        } else {
            // exact
            if(strcmp(fd.cFileName, pattern)==0) match=true;
        }

        if(match && !is_dir){
            char entry[XZERO_PATH_MAX+2];
            snprintf(entry,sizeof(entry),"%s\n", full);
            size_t el=strlen(entry);
            if(*len + el < cap -1 && *count < GLOB_MAX){
                memcpy(out+*len, entry, el);
                *len+=el;
                (*count)++;
                if(*count >= GLOB_MAX){ *stop=true; }
            }
        }
        if(is_dir){
            // skip build, .git, .xzero
            if(strcmp(fd.cFileName,".git")==0||strcmp(fd.cFileName,".xzero")==0||strcmp(fd.cFileName,"build")==0) continue;
            glob_win_rec(full, pattern, out, len, cap, count, stop);
        }
    }while(FindNextFileA(h,&fd));
    FindClose(h);
}
#else
static void glob_posix_rec(const char *base, const char *pattern, char *out, size_t *len, size_t cap, int *count, bool *stop){
    if(*stop) return;
    DIR *d=opendir(base);
    if(!d) return;
    struct dirent *ent;
    while((ent=readdir(d))){
        if(strcmp(ent->d_name,".")==0||strcmp(ent->d_name,"..")==0) continue;
        if(*stop) break;
        char full[XZERO_PATH_MAX];
        util_path_join(full,sizeof(full), base, ent->d_name);
        struct stat st;
        if(stat(full,&st)!=0) continue;
        bool is_dir=S_ISDIR(st.st_mode);
        bool match=false;
        if(fnmatch(pattern, ent->d_name, 0)==0) match=true;
        else if(fnmatch(pattern, full, 0)==0) match=true;
        else {
            // try basename match for patterns like **/*.c
            const char *base_pat=strrchr(pattern,'/');
            if(base_pat) {
                if(fnmatch(base_pat+1, ent->d_name,0)==0) match=true;
            }
        }
        if(match && !is_dir){
            char entry[XZERO_PATH_MAX+2];
            snprintf(entry,sizeof(entry),"%s\n", full);
            size_t el=strlen(entry);
            if(*len + el < cap -1 && *count < GLOB_MAX){
                memcpy(out+*len, entry, el);
                *len+=el;
                (*count)++;
                if(*count>=GLOB_MAX) *stop=true;
            }
        }
        if(is_dir){
            if(strcmp(ent->d_name,".git")==0||strcmp(ent->d_name,".xzero")==0||strcmp(ent->d_name,"build")==0||strcmp(ent->d_name,"node_modules")==0) continue;
            glob_posix_rec(full, pattern, out, len, cap, count, stop);
        }
    }
    closedir(d);
}
#endif

ToolResult tool_glob(cJSON *args, Session *session){
    (void)session;
    ToolResult r={0};
    const char *pattern=NULL;
    const char *path=".";
    if(args){
        cJSON *p=cJSON_GetObjectItem(args,"pattern");
        if(cJSON_IsString(p)) pattern=p->valuestring;
        cJSON *pa=cJSON_GetObjectItem(args,"path");
        if(cJSON_IsString(pa)) path=pa->valuestring;
    }
    if(!pattern||!pattern[0]){ r.content=strdup("error: missing 'pattern'"); r.is_error=true; return r; }

    size_t cap=GLOB_CAP;
    char *out=(char*)malloc(cap);
    if(!out){ r.content=strdup("error: oom"); r.is_error=true; return r; }
    size_t len=0;
    out[0]='\0';
    int count=0;
    bool stop=false;

#ifdef _WIN32
    glob_win_rec(path, pattern, out, &len, cap, &count, &stop);
#else
    glob_posix_rec(path, pattern, out, &len, cap, &count, &stop);
#endif
    out[len]='\0';
    if(len==0){
        free(out);
        r.content=strdup("no files matched");
        return r;
    }
    if(stop){
        const char *footer="\n[200 files, stopping. Refine pattern.]\n";
        size_t fl=strlen(footer);
        if(len+fl < cap){ memcpy(out+len, footer, fl); len+=fl; out[len]='\0'; }
    }
    bool trunc=false;
    size_t lines=0;
    char spill[XZERO_PATH_MAX];
    util_temp_dir(spill,sizeof(spill));
    util_path_join(spill,sizeof(spill), spill, "xzero_glob_full.txt");
    char *capped=util_cap_output(out,len,spill,&trunc,&lines);
    free(out);
    r.content=capped;
    r.truncated=trunc;
    if(trunc) r.full_path=strdup(spill);
    printf("[xzero] glob '%s' -> %d files%s\n", pattern, count, stop?" (capped)":"");
    fflush(stdout);
    return r;
}
