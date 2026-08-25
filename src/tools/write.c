#include "registry.h"
#include "../util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>

ToolResult tool_write(cJSON *args, Session *session) {
    ToolResult r={0};
    const char *path=NULL;
    const char *content=NULL;
    if (args) {
        cJSON *p=cJSON_GetObjectItem(args,"path");
        if(cJSON_IsString(p)) path=p->valuestring;
        cJSON *c=cJSON_GetObjectItem(args,"content");
        if(cJSON_IsString(c)) content=c->valuestring;
    }
    if(!path||!path[0]){ r.content=strdup("error: missing 'path'"); r.is_error=true; return r; }
    if(!content) content="";
    if(session) session_add_touched(session, path);

    // Ensure dir exists
    char dir[XZERO_PATH_MAX];
    strncpy(dir, path, sizeof(dir)-1);
    dir[sizeof(dir)-1]='\0';
    char *sep=strrchr(dir,'/');
    char *sep2=strrchr(dir,'\\');
    if(sep2 && (!sep||sep2>sep)) sep=sep2;
    if(sep){ *sep='\0'; util_mkdir_p(dir); }

    FILE *f=fopen(path,"wb");
    if(!f){ char buf[1024]; snprintf(buf,sizeof(buf),"error: cannot write '%s'",path); r.content=strdup(buf); r.is_error=true; return r; }
    size_t len=strlen(content);
    size_t w=fwrite(content,1,len,f);
    fclose(f);
    if(w!=len){ r.content=strdup("error: short write"); r.is_error=true; return r; }

    // Lever #1: edit/write returns receipt, not full content. Human gets diff via stdout elsewhere.
    // For write, model gets exactly one line: success
    char buf[512];
    snprintf(buf,sizeof(buf),"Successfully wrote %zu bytes to %s", len, path);
    r.content=strdup(buf);
    // Human detail: we print full content preview to stdout? The caller (agent) will log to human channel separately.
    // But for API, we keep receipt minimal.
    printf("[xzero] Wrote %s (%zu bytes)\n", path, len);
    fflush(stdout);
    return r;
}
