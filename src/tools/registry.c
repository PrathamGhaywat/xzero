#include "registry.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <cJSON.h>

// Forward declarations
ToolResult tool_read(cJSON *args, Session *session);
ToolResult tool_write(cJSON *args, Session *session);
ToolResult tool_edit(cJSON *args, Session *session);
ToolResult tool_bash(cJSON *args, Session *session);
ToolResult tool_grep(cJSON *args, Session *session);
ToolResult tool_glob(cJSON *args, Session *session);

void tool_result_free(ToolResult *r){
    if(!r) return;
    free(r->content);
    free(r->full_path);
    r->content=NULL;
    r->full_path=NULL;
}

static ToolDef defs[] = {
    {"read",  "Read a file with paging (offset, limit). Returns bounded preview.", tool_read, NULL},
    {"grep",  "Search for regex pattern in files. Caps at 100 matches.", tool_grep, NULL},
    {"glob",  "Find files by pattern. Caps at 200 files.", tool_glob, NULL},
    {"write", "Write file (create or overwrite). Returns receipt.", tool_write, NULL},
    {"edit",  "Edit file by replacing exact string. Returns receipt.", tool_edit, NULL},
    {"bash",  "Execute shell command. Returns capped output (50KB/2000 lines).", tool_bash, NULL},
};
static int ndefs = sizeof(defs)/sizeof(defs[0]);

static void ensure_schemas(void){
    static bool done=false;
    if(done) return;
    done=true;
    // read
    {
        cJSON *p=cJSON_CreateObject();
        cJSON_AddStringToObject(p,"type","object");
        cJSON *props=cJSON_CreateObject();
        cJSON *path=cJSON_CreateObject(); cJSON_AddStringToObject(path,"type","string"); cJSON_AddStringToObject(path,"description","File path");
        cJSON_AddItemToObject(props,"path",path);
        cJSON *off=cJSON_CreateObject(); cJSON_AddStringToObject(off,"type","number"); cJSON_AddStringToObject(off,"description","Offset line (0-indexed)");
        cJSON_AddItemToObject(props,"offset",off);
        cJSON *lim=cJSON_CreateObject(); cJSON_AddStringToObject(lim,"type","number"); cJSON_AddStringToObject(lim,"description","Max lines");
        cJSON_AddItemToObject(props,"limit",lim);
        cJSON_AddItemToObject(p,"properties",props);
        cJSON *req=cJSON_CreateArray(); cJSON_AddItemToArray(req,cJSON_CreateString("path"));
        cJSON_AddItemToObject(p,"required",req);
        defs[0].parameters_schema=p;
    }
    // grep
    {
        cJSON *p=cJSON_CreateObject();
        cJSON_AddStringToObject(p,"type","object");
        cJSON *props=cJSON_CreateObject();
        cJSON *pat=cJSON_CreateObject(); cJSON_AddStringToObject(pat,"type","string"); cJSON_AddStringToObject(pat,"description","Regex pattern");
        cJSON_AddItemToObject(props,"pattern",pat);
        cJSON *pa=cJSON_CreateObject(); cJSON_AddStringToObject(pa,"type","string"); cJSON_AddStringToObject(pa,"description","Path to search");
        cJSON_AddItemToObject(props,"path",pa);
        cJSON_AddItemToObject(p,"properties",props);
        cJSON *req=cJSON_CreateArray(); cJSON_AddItemToArray(req,cJSON_CreateString("pattern"));
        cJSON_AddItemToObject(p,"required",req);
        defs[1].parameters_schema=p;
    }
    // glob
    {
        cJSON *p=cJSON_CreateObject();
        cJSON_AddStringToObject(p,"type","object");
        cJSON *props=cJSON_CreateObject();
        cJSON *pat=cJSON_CreateObject(); cJSON_AddStringToObject(pat,"type","string"); cJSON_AddStringToObject(pat,"description","Glob pattern");
        cJSON_AddItemToObject(props,"pattern",pat);
        cJSON *pa=cJSON_CreateObject(); cJSON_AddStringToObject(pa,"type","string"); cJSON_AddStringToObject(pa,"description","Base path");
        cJSON_AddItemToObject(props,"path",pa);
        cJSON_AddItemToObject(p,"properties",props);
        cJSON *req=cJSON_CreateArray(); cJSON_AddItemToArray(req,cJSON_CreateString("pattern"));
        cJSON_AddItemToObject(p,"required",req);
        defs[2].parameters_schema=p;
    }
    // write
    {
        cJSON *p=cJSON_CreateObject();
        cJSON_AddStringToObject(p,"type","object");
        cJSON *props=cJSON_CreateObject();
        cJSON *pa=cJSON_CreateObject(); cJSON_AddStringToObject(pa,"type","string"); cJSON_AddStringToObject(pa,"description","File path");
        cJSON_AddItemToObject(props,"path",pa);
        cJSON *co=cJSON_CreateObject(); cJSON_AddStringToObject(co,"type","string"); cJSON_AddStringToObject(co,"description","File content");
        cJSON_AddItemToObject(props,"content",co);
        cJSON_AddItemToObject(p,"properties",props);
        cJSON *req=cJSON_CreateArray(); cJSON_AddItemToArray(req,cJSON_CreateString("path")); cJSON_AddItemToArray(req,cJSON_CreateString("content"));
        cJSON_AddItemToObject(p,"required",req);
        defs[3].parameters_schema=p;
    }
    // edit
    {
        cJSON *p=cJSON_CreateObject();
        cJSON_AddStringToObject(p,"type","object");
        cJSON *props=cJSON_CreateObject();
        cJSON *pa=cJSON_CreateObject(); cJSON_AddStringToObject(pa,"type","string");
        cJSON_AddItemToObject(props,"path",pa);
        cJSON *os=cJSON_CreateObject(); cJSON_AddStringToObject(os,"type","string"); cJSON_AddStringToObject(os,"description","Exact string to replace");
        cJSON_AddItemToObject(props,"old_string",os);
        cJSON *ns=cJSON_CreateObject(); cJSON_AddStringToObject(ns,"type","string"); cJSON_AddStringToObject(ns,"description","New string");
        cJSON_AddItemToObject(props,"new_string",ns);
        cJSON *ra=cJSON_CreateObject(); cJSON_AddStringToObject(ra,"type","boolean");
        cJSON_AddItemToObject(props,"replace_all",ra);
        cJSON_AddItemToObject(p,"properties",props);
        cJSON *req=cJSON_CreateArray(); cJSON_AddItemToArray(req,cJSON_CreateString("path")); cJSON_AddItemToArray(req,cJSON_CreateString("old_string")); cJSON_AddItemToArray(req,cJSON_CreateString("new_string"));
        cJSON_AddItemToObject(p,"required",req);
        defs[4].parameters_schema=p;
    }
    // bash
    {
        cJSON *p=cJSON_CreateObject();
        cJSON_AddStringToObject(p,"type","object");
        cJSON *props=cJSON_CreateObject();
        cJSON *co=cJSON_CreateObject(); cJSON_AddStringToObject(co,"type","string"); cJSON_AddStringToObject(co,"description","Shell command");
        cJSON_AddItemToObject(props,"command",co);
        cJSON *to=cJSON_CreateObject(); cJSON_AddStringToObject(to,"type","number"); cJSON_AddStringToObject(to,"description","Timeout ms");
        cJSON_AddItemToObject(props,"timeout",to);
        cJSON_AddItemToObject(p,"properties",props);
        cJSON *req=cJSON_CreateArray(); cJSON_AddItemToArray(req,cJSON_CreateString("command"));
        cJSON_AddItemToObject(p,"required",req);
        defs[5].parameters_schema=p;
    }
}

int tools_count(void){ ensure_schemas(); return ndefs; }
const ToolDef *tools_get(int idx){ ensure_schemas(); if(idx<0||idx>=ndefs) return NULL; return &defs[idx]; }
const ToolDef *tools_find(const char *name){
    ensure_schemas();
    if(!name) return NULL;
    for(int i=0;i<ndefs;i++) if(strcmp(defs[i].name,name)==0) return &defs[i];
    return NULL;
}

ToolResult tools_dispatch(const char *name, const char *args_json_str, Session *session){
    const ToolDef *def=tools_find(name);
    if(!def){
        ToolResult r={0};
        char buf[512];
        snprintf(buf,sizeof(buf),"error: unknown tool '%s'", name?name:"(null)");
        r.content=strdup(buf);
        r.is_error=true;
        return r;
    }
    cJSON *args=NULL;
    if(args_json_str && args_json_str[0]){
        args=cJSON_Parse(args_json_str);
        if(!args){
            ToolResult r={0};
            r.content=strdup("error: invalid JSON args");
            r.is_error=true;
            return r;
        }
    } else {
        args=cJSON_CreateObject();
    }
    ToolResult res=def->fn(args, session);
    cJSON_Delete(args);
    return res;
}

cJSON *tools_build_openai_array(void){
    ensure_schemas();
    cJSON *arr=cJSON_CreateArray();
    for(int i=0;i<ndefs;i++){
        cJSON *tool=cJSON_CreateObject();
        cJSON_AddStringToObject(tool,"type","function");
        cJSON *func=cJSON_CreateObject();
        cJSON_AddStringToObject(func,"name",defs[i].name);
        cJSON_AddStringToObject(func,"description",defs[i].description);
        cJSON_AddItemToObject(func,"parameters", cJSON_Duplicate(defs[i].parameters_schema,1));
        cJSON_AddItemToObject(tool,"function",func);
        cJSON_AddItemToArray(arr, tool);
    }
    return arr;
}

bool tools_needs_approval(const char *name){
    if(!name) return true;
    // read/grep/glob auto-approved, write/edit/bash need approval
    if(strcmp(name,"read")==0||strcmp(name,"grep")==0||strcmp(name,"glob")==0) return false;
    return true;
}
