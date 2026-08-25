#include "registry.h"
#include "../util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>

ToolResult tool_edit(cJSON *args, Session *session) {
    ToolResult r={0};
    const char *path=NULL;
    const char *oldStr=NULL;
    const char *newStr=NULL;
    bool replaceAll=false;
    if(args){
        cJSON *p=cJSON_GetObjectItem(args,"path");
        if(cJSON_IsString(p)) path=p->valuestring;
        cJSON *o=cJSON_GetObjectItem(args,"old_string");
        if(!o) o=cJSON_GetObjectItem(args,"oldString");
        if(cJSON_IsString(o)) oldStr=o->valuestring;
        cJSON *n=cJSON_GetObjectItem(args,"new_string");
        if(!n) n=cJSON_GetObjectItem(args,"newString");
        if(cJSON_IsString(n)) newStr=n->valuestring;
        cJSON *ra=cJSON_GetObjectItem(args,"replace_all");
        if(cJSON_IsBool(ra)) replaceAll=cJSON_IsTrue(ra);
    }
    if(!path||!path[0]){ r.content=strdup("error: missing 'path'"); r.is_error=true; return r; }
    if(!oldStr) oldStr="";
    if(!newStr) newStr="";
    if(session) session_add_touched(session, path);

    FILE *f=fopen(path,"rb");
    if(!f){ char buf[1024]; snprintf(buf,sizeof(buf),"error: cannot open '%s' for edit",path); r.content=strdup(buf); r.is_error=true; return r; }
    fseek(f,0,SEEK_END);
    long sz=ftell(f);
    fseek(f,0,SEEK_SET);
    char *data=(char*)malloc(sz+1);
    if(!data){ fclose(f); r.content=strdup("error: oom"); r.is_error=true; return r; }
    size_t got=fread(data,1,sz,f);
    fclose(f);
    data[got]='\0';

    // Count occurrences
    int count=0;
    const char *pos=data;
    size_t oldLen=strlen(oldStr);
    if(oldLen==0){
        free(data);
        r.content=strdup("error: old_string empty - use write for new files");
        r.is_error=true;
        return r;
    }
    while((pos=strstr(pos, oldStr))){ count++; pos+=oldLen; if(!replaceAll) break; }

    if(count==0){
        free(data);
        r.content=strdup("error: old_string not found in file");
        r.is_error=true;
        return r;
    }
    if(!replaceAll && count>1){
        // Need to check if actually multiple matches - we only counted 1 if not replaceAll, need full
        int total=0;
        pos=data;
        while((pos=strstr(pos, oldStr))){ total++; pos+=oldLen; }
        if(total>1){
            free(data);
            r.content=strdup("error: old_string found multiple times - provide more context or use replace_all:true");
            r.is_error=true;
            return r;
        }
    }

    // Build new content
    size_t newLen=strlen(newStr);
    size_t newSize;
    if(replaceAll) {
        // estimate
        newSize = got + count * (newLen - oldLen) + 1;
    } else {
        newSize = got + (newLen - oldLen) + 1;
    }
    char *out=(char*)malloc(newSize);
    if(!out){ free(data); r.content=strdup("error: oom"); r.is_error=true; return r; }
    char *dst=out;
    const char *src=data;
    int replaced=0;
    while(*src){
        const char *found=strstr(src, oldStr);
        if(!found || (!replaceAll && replaced>=1)){
            strcpy(dst, src);
            break;
        }
        size_t copyLen=found - src;
        memcpy(dst, src, copyLen);
        dst+=copyLen;
        memcpy(dst, newStr, newLen);
        dst+=newLen;
        src=found+oldLen;
        replaced++;
    }
    *dst='\0';

    FILE *wf=fopen(path,"wb");
    if(!wf){ free(data); free(out); r.content=strdup("error: cannot write back"); r.is_error=true; return r; }
    fwrite(out,1,strlen(out),wf);
    fclose(wf);
    free(data);
    free(out);

    char buf[512];
    snprintf(buf,sizeof(buf),"Successfully replaced %d block(s) in %s", count, path);
    r.content=strdup(buf);
    // Human gets diff preview - print to stdout (not to model)
    printf("[xzero] Edit %s: %d replacement(s)\n", path, count);
    fflush(stdout);
    return r;
}
