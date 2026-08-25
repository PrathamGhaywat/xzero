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
#include <regex.h>
#include <sys/stat.h>
#endif

#define GREP_MAX_MATCHES 100
#define GREP_CAP_BYTES (50*1024)

// Simple grep: recursive, 100 matches stopping (Lever #1)
static bool is_text_file(const char *path){
    const char *ext=strrchr(path,'.');
    if(!ext) return true;
    // skip binaries
    if(strcmp(ext,".exe")==0||strcmp(ext,".dll")==0||strcmp(ext,".png")==0||strcmp(ext,".jpg")==0||strcmp(ext,".zip")==0) return false;
    return true;
}

#ifdef _WIN32
static void grep_win(const char *pattern, const char *dir, char *out, size_t *out_len, size_t out_cap, int *matches, bool *stopped){
    if(*stopped) return;
    char search[XZERO_PATH_MAX];
    util_path_join(search,sizeof(search), dir, "*");
    WIN32_FIND_DATAA fd;
    HANDLE h=FindFirstFileA(search,&fd);
    if(h==INVALID_HANDLE_VALUE) return;
    do{
        if(strcmp(fd.cFileName,".")==0||strcmp(fd.cFileName,"..")==0) continue;
        if(*stopped) break;
        char full[XZERO_PATH_MAX];
        util_path_join(full,sizeof(full), dir, fd.cFileName);
        if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){
            grep_win(pattern, full, out, out_len, out_cap, matches, stopped);
        } else {
            if(!is_text_file(full)) continue;
            FILE *f=fopen(full,"rb");
            if(!f) continue;
            fseek(f,0,SEEK_END);
            long sz=ftell(f);
            fseek(f,0,SEEK_SET);
            if(sz>1024*1024) { fclose(f); continue; } // skip large
            char *buf=(char*)malloc(sz+1);
            if(!buf){ fclose(f); continue; }
            size_t got=fread(buf,1,sz,f);
            fclose(f);
            buf[got]='\0';
            // naive substring search (case-sensitive)
            char *pos=buf;
            int line_no=1;
            char *line_start=buf;
            for(size_t i=0;i<got;i++){
                if(buf[i]=='\n'){
                    size_t line_len = &buf[i] - line_start;
                    char *line=(char*)malloc(line_len+1);
                    if(line){
                        memcpy(line,line_start,line_len);
                        line[line_len]='\0';
                        if(strstr(line, pattern)){
                            char entry[1024];
                            snprintf(entry,sizeof(entry),"%s:%d:%s\n", full, line_no, line);
                            size_t el=strlen(entry);
                            if(*out_len + el < out_cap -1){
                                memcpy(out+*out_len, entry, el);
                                *out_len+=el;
                                (*matches)++;
                                if(*matches >= GREP_MAX_MATCHES){
                                    const char *msg="\n[100 matches, stopping. Refine pattern.]\n";
                                    size_t ml=strlen(msg);
                                    if(*out_len+ml < out_cap) { memcpy(out+*out_len, msg, ml); *out_len+=ml; }
                                    *stopped=true;
                                    free(line);
                                    free(buf);
                                    return;
                                }
                            }
                        }
                        free(line);
                    }
                    line_start=&buf[i+1];
                    line_no++;
                }
            }
            free(buf);
        }
    }while(FindNextFileA(h,&fd));
    FindClose(h);
}
#else
static void grep_posix_dir(const char *pattern, const char *dir, regex_t *re, char *out, size_t *out_len, size_t out_cap, int *matches, bool *stopped){
    if(*stopped) return;
    DIR *d=opendir(dir);
    if(!d) return;
    struct dirent *ent;
    while((ent=readdir(d))){
        if(strcmp(ent->d_name,".")==0||strcmp(ent->d_name,"..")==0) continue;
        if(*stopped) break;
        char full[XZERO_PATH_MAX];
        util_path_join(full,sizeof(full), dir, ent->d_name);
        struct stat st;
        if(stat(full,&st)!=0) continue;
        if(S_ISDIR(st.st_mode)){
            // skip .git, .xzero, build, node_modules for efficiency
            if(strcmp(ent->d_name,".git")==0||strcmp(ent->d_name,".xzero")==0||strcmp(ent->d_name,"build")==0||strcmp(ent->d_name,"node_modules")==0) continue;
            grep_posix_dir(pattern, full, re, out, out_len, out_cap, matches, stopped);
        } else {
            if(!is_text_file(full)) continue;
            if(st.st_size > 1024*1024) continue;
            FILE *f=fopen(full,"rb");
            if(!f) continue;
            char line[4096];
            int line_no=1;
            while(fgets(line,sizeof(line),f)){
                int rc = regexec(re, line, 0, NULL, 0);
                if(rc==0){
                    // trim newline
                    size_t ll=strlen(line);
                    while(ll>0 && (line[ll-1]=='\n'||line[ll-1]=='\r')) line[--ll]='\0';
                    char entry[8192];
                    snprintf(entry,sizeof(entry),"%s:%d:%s\n", full, line_no, line);
                    size_t el=strlen(entry);
                    if(*out_len + el < out_cap -1){
                        memcpy(out+*out_len, entry, el);
                        *out_len+=el;
                        (*matches)++;
                        if(*matches >= GREP_MAX_MATCHES){
                            const char *msg="\n[100 matches, stopping. Refine pattern.]\n";
                            size_t ml=strlen(msg);
                            if(*out_len+ml < out_cap) { memcpy(out+*out_len, msg, ml); *out_len+=ml; }
                            *stopped=true;
                            break;
                        }
                    } else {
                        *stopped=true; break;
                    }
                }
                line_no++;
            }
            fclose(f);
        }
    }
    closedir(d);
}
#endif

ToolResult tool_grep(cJSON *args, Session *session){
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

    size_t cap=GREP_CAP_BYTES+4096;
    char *out=(char*)malloc(cap);
    if(!out){ r.content=strdup("error: oom"); r.is_error=true; return r; }
    size_t out_len=0;
    out[0]='\0';
    int matches=0;
    bool stopped=false;

#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    bool is_file = (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
    if(is_file){
        // file
        FILE *f=fopen(path,"rb");
        if(f){
            char line[4096];
            int line_no=1;
            while(fgets(line,sizeof(line),f) && !stopped){
                if(strstr(line, pattern)){
                    char entry[8192];
                    size_t ll=strlen(line);
                    while(ll>0 && (line[ll-1]=='\n'||line[ll-1]=='\r')) line[--ll]='\0';
                    snprintf(entry,sizeof(entry),"%s:%d:%s\n", path, line_no, line);
                    size_t el=strlen(entry);
                    if(out_len+el < cap -1){ memcpy(out+out_len, entry, el); out_len+=el; matches++; if(matches>=GREP_MAX_MATCHES) stopped=true; }
                }
                line_no++;
            }
            fclose(f);
        }
    } else {
        grep_win(pattern, path, out, &out_len, cap, &matches, &stopped);
    }
#else
    regex_t re;
    int rc=regcomp(&re, pattern, REG_EXTENDED);
    if(rc!=0){
        char errbuf[256];
        regerror(rc,&re,errbuf,sizeof(errbuf));
        char buf[512];
        snprintf(buf,sizeof(buf),"error: invalid regex '%s': %s", pattern, errbuf);
        free(out);
        r.content=strdup(buf); r.is_error=true; return r;
    }
    struct stat st;
    if(stat(path,&st)==0 && S_ISREG(st.st_mode)){
        FILE *f=fopen(path,"rb");
        if(f){
            char line[4096];
            int line_no=1;
            while(fgets(line,sizeof(line),f) && !stopped){
                if(regexec(&re,line,0,NULL,0)==0){
                    size_t ll=strlen(line);
                    while(ll>0 && (line[ll-1]=='\n'||line[ll-1]=='\r')) line[--ll]='\0';
                    char entry[8192];
                    snprintf(entry,sizeof(entry),"%s:%d:%s\n", path, line_no, line);
                    size_t el=strlen(entry);
                    if(out_len+el < cap -1){ memcpy(out+out_len, entry, el); out_len+=el; matches++; if(matches>=GREP_MAX_MATCHES) stopped=true; }
                }
                line_no++;
            }
            fclose(f);
        }
    } else {
        grep_posix_dir(pattern, path, &re, out, &out_len, cap, &matches, &stopped);
    }
    regfree(&re);
#endif
    out[out_len]='\0';
    if(out_len==0){
        free(out);
        r.content=strdup("no matches");
        return r;
    }
    // Cap for model (though grep already capped)
    bool trunc=false;
    size_t lines=0;
    char spill[XZERO_PATH_MAX];
    util_temp_dir(spill,sizeof(spill));
    util_path_join(spill,sizeof(spill), spill, "xzero_grep_full.txt");
    char *capped=util_cap_output(out, out_len, spill, &trunc, &lines);
    free(out);
    r.content=capped;
    r.truncated=trunc;
    if(trunc){
        r.full_path=strdup(spill);
        FILE *sf=fopen(spill,"wb");
        if(sf){ fwrite(capped,1,strlen(capped),sf); fclose(sf); }
    }
    // Human sees count
    printf("[xzero] grep '%s' -> %d matches%s\n", pattern, matches, stopped?" (capped at 100)":"");
    fflush(stdout);
    return r;
}
