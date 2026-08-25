#include "registry.h"
#include "../util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <cJSON.h>

// Lever #1: Read pages file, doesn't inline whole file unbounded.
// Args: { "path": "src/main.c", "offset": 0, "limit": 200 }
ToolResult tool_read(cJSON *args, Session *session) {
    ToolResult r={0};
    const char *path = NULL;
    int offset=0, limit=200;
    if (args) {
        cJSON *p = cJSON_GetObjectItem(args, "path");
        if (cJSON_IsString(p)) path=p->valuestring;
        cJSON *o = cJSON_GetObjectItem(args, "offset");
        if (cJSON_IsNumber(o)) offset=o->valueint;
        cJSON *l = cJSON_GetObjectItem(args, "limit");
        if (cJSON_IsNumber(l)) limit=l->valueint;
    }
    if (!path || !path[0]) {
        r.content=strdup("error: missing 'path'");
        r.is_error=true;
        return r;
    }
    if (limit<=0) limit=200;
    if (limit>500) limit=500; // bound
    if (session) session_add_touched(session, path);

    FILE *f = fopen(path, "rb");
    if (!f) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "error: cannot open '%s'", path);
        r.content=strdup(buf);
        r.is_error=true;
        return r;
    }
    fseek(f,0,SEEK_END);
    long sz=ftell(f);
    fseek(f,0,SEEK_SET);
    char *data=(char*)malloc(sz+1);
    if (!data) { fclose(f); r.content=strdup("error: oom"); r.is_error=true; return r; }
    size_t got=fread(data,1,sz,f);
    fclose(f);
    data[got]='\0';

    // Paging logic (Lever #1: read pages)
    int total_lines=0;
    for (size_t i=0;i<got;i++) if(data[i]=='\n') total_lines++;
    // find offset
    int cur_line=0;
    size_t start=0;
    for (size_t i=0;i<got && cur_line<offset;i++) {
        if (data[i]=='\n') cur_line++;
        if (cur_line==offset) { start=i+1; break; }
    }
    if (offset>0 && cur_line<offset) {
        free(data);
        char buf[512];
        snprintf(buf,sizeof(buf),"warning: offset %d beyond file (%d lines)", offset, total_lines);
        r.content=strdup(buf);
        return r;
    }
    // collect limit lines
    size_t end=start;
    int lines_collected=0;
    for (size_t i=start;i<got && lines_collected<limit;i++) {
        if (data[i]=='\n') lines_collected++;
        end=i+1;
    }
    // if not limited, end remains
    if (lines_collected<limit) end=got;

    size_t slice_len = end - start;
    char *slice=(char*)malloc(slice_len+512);
    if (!slice) { free(data); r.content=strdup("error: oom"); r.is_error=true; return r; }
    memcpy(slice, data+start, slice_len);
    slice[slice_len]='\0';
    free(data);

    // Add footer if paged
    bool has_more = (end < got);
    if (has_more) {
        char footer[256];
        // count total lines more accurately for footer
        snprintf(footer,sizeof(footer),"\n[File %s: showing lines %d-%d of ~%d. Use offset %d to page.]\n", path, offset+1, offset+lines_collected, total_lines+1, offset+limit);
        strcat(slice, footer);
    }
    // Cap output for model (50KB/2000 lines) - but read already limited, just ensure
    bool trunc=false;
    size_t out_lines=0;
    char spill[XZERO_PATH_MAX];
    util_temp_dir(spill,sizeof(spill));
    util_path_join(spill,sizeof(spill), spill, "xzero_read_full.txt");
    char *capped = util_cap_output(slice, strlen(slice), spill, &trunc, &out_lines);
    free(slice);
    r.content=capped;
    r.truncated=trunc;
    if (trunc) {
        r.full_path=strdup(spill);
        // spill full slice
        FILE *sf=fopen(spill,"wb");
        if(sf){ fwrite(capped,1,strlen(capped),sf); fclose(sf); }
    }
    return r;
}
