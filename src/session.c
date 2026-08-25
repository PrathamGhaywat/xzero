#include "session.h"
#include "util.h"
#include "openai.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <cJSON.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#include <dirent.h>
#endif

#define COMPACT_WINDOW_TOKENS 20000
#define COMPACT_KEEP_RECENT  8000  // keep last 8k tokens verbatim

void session_get_dir(char *out, size_t n) {
    // Prefer ./.xzero/sessions if project-local exists or cwd is project
    struct stat st;
    if (stat("./.xzero", &st)==0 && (st.st_mode & S_IFDIR)) {
        util_path_join(out, n, "./.xzero", "sessions");
        return;
    }
    // global
    char global[XZERO_PATH_MAX];
    // reuse config path logic but for sessions
#ifdef _WIN32
    char *appdata = getenv("APPDATA");
    if (appdata) {
        util_path_join(global, sizeof(global), appdata, "xzero/sessions");
        util_str_copy(out, n, global);
        return;
    }
    util_str_copy(out, n, "./.xzero/sessions");
#else
    const char *home = getenv("HOME");
    if (!home) home = ".";
    char cfg[XZERO_PATH_MAX];
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0]) util_path_join(cfg, sizeof(cfg), xdg, "xzero/sessions");
    else util_path_join(cfg, sizeof(cfg), home, ".local/share/xzero/sessions");
    // fallback to ~/.xzero/sessions if .local not exists
    util_str_copy(out, n, cfg);
#endif
}

void session_get_path(const char *id, char *out, size_t n) {
    char dir[XZERO_PATH_MAX];
    session_get_dir(dir, sizeof(dir));
    char fname[XZERO_PATH_MAX];
    snprintf(fname, sizeof(fname), "%s.jsonl", id);
    util_path_join(out, n, dir, fname);
}

Session *session_create(const char *model) {
    Session *s = (Session*)calloc(1, sizeof(Session));
    if (!s) return NULL;
    util_generate_id(s->id, sizeof(s->id));
    if (model) util_str_copy(s->model, sizeof(s->model), model);
    s->messages = cJSON_CreateArray();
    // Add system prompt as first message (cache breakpoint 1)
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", openai_system_prompt());
    cJSON_AddItemToArray(s->messages, sys);
    s->estimated_tokens = util_estimate_tokens(openai_system_prompt());

    char dir[XZERO_PATH_MAX];
    session_get_dir(dir, sizeof(dir));
    util_mkdir_p(dir);
    session_get_path(s->id, s->file_path, sizeof(s->file_path));
    s->summary[0]='\0';
    return s;
}

void session_free(Session *s) {
    if (!s) return;
    if (s->messages) cJSON_Delete(s->messages);
    for (int i=0;i<s->touched_count;i++) free(s->touched_files[i]);
    free(s);
}

bool session_save(Session *s) {
    if (!s || !s->file_path[0]) return false;
    char dir[XZERO_PATH_MAX];
    strncpy(dir, s->file_path, sizeof(dir)-1);
    dir[sizeof(dir)-1]='\0';
    char *sep = strrchr(dir, '/');
    char *sep2 = strrchr(dir, '\\');
    if (sep2 && (!sep || sep2>sep)) sep=sep2;
    if (sep) { *sep='\0'; util_mkdir_p(dir); }

    // Write JSONL: one JSON per line (messages + metadata)
    FILE *f = fopen(s->file_path, "wb");
    if (!f) return false;
    // metadata line
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "type", "meta");
    cJSON_AddStringToObject(meta, "id", s->id);
    cJSON_AddStringToObject(meta, "model", s->model);
    cJSON_AddStringToObject(meta, "summary", s->summary);
    // touched files as array
    cJSON *tf = cJSON_CreateArray();
    for (int i=0;i<s->touched_count;i++) cJSON_AddItemToArray(tf, cJSON_CreateString(s->touched_files[i]));
    cJSON_AddItemToObject(meta, "touched_files", tf);
    char *meta_str = cJSON_PrintUnformatted(meta);
    cJSON_Delete(meta);
    if (meta_str) { fprintf(f, "%s\n", meta_str); free(meta_str); }

    // messages: each as line
    int n = cJSON_GetArraySize(s->messages);
    for (int i=0;i<n;i++) {
        cJSON *msg = cJSON_GetArrayItem(s->messages, i);
        char *line = cJSON_PrintUnformatted(msg);
        if (line) { fprintf(f, "%s\n", line); free(line); }
    }
    fclose(f);
#ifndef _WIN32
    // also save summary separately for quick load
#endif
    return true;
}

bool session_load(Session *s, const char *id_or_path) {
    if (!s || !id_or_path) return false;
    char path[XZERO_PATH_MAX];
    if (strchr(id_or_path, '/') || strchr(id_or_path, '\\') || strstr(id_or_path, ".json")) {
        util_str_copy(path, sizeof(path), id_or_path);
    } else {
        session_get_path(id_or_path, path, sizeof(path));
    }
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    util_str_copy(s->file_path, sizeof(s->file_path), path);
    if (s->messages) cJSON_Delete(s->messages);
    s->messages = cJSON_CreateArray();
    s->touched_count=0;
    s->summary[0]='\0';
    s->estimated_tokens=0;

    char line[64*1024];
    bool first=true;
    while (fgets(line, sizeof(line), f)) {
        size_t len=strlen(line);
        while(len>0 && (line[len-1]=='\n' || line[len-1]=='\r')) line[--len]='\0';
        if (len==0) continue;
        cJSON *obj = cJSON_Parse(line);
        if (!obj) continue;
        if (first) {
            cJSON *type = cJSON_GetObjectItem(obj, "type");
            if (cJSON_IsString(type) && strcmp(type->valuestring,"meta")==0) {
                cJSON *id = cJSON_GetObjectItem(obj, "id");
                cJSON *model = cJSON_GetObjectItem(obj, "model");
                cJSON *summary = cJSON_GetObjectItem(obj, "summary");
                cJSON *tf = cJSON_GetObjectItem(obj, "touched_files");
                if (cJSON_IsString(id)) util_str_copy(s->id, sizeof(s->id), id->valuestring);
                if (cJSON_IsString(model)) util_str_copy(s->model, sizeof(s->model), model->valuestring);
                if (cJSON_IsString(summary)) util_str_copy(s->summary, sizeof(s->summary), summary->valuestring);
                if (cJSON_IsArray(tf)) {
                    int tn = cJSON_GetArraySize(tf);
                    for (int i=0;i<tn && s->touched_count<256;i++) {
                        cJSON *item = cJSON_GetArrayItem(tf, i);
                        if (cJSON_IsString(item)) s->touched_files[s->touched_count++] = strdup(item->valuestring);
                    }
                }
                cJSON_Delete(obj);
                first=false;
                continue;
            }
        }
        first=false;
        // estimate tokens
        char *tmp = cJSON_PrintUnformatted(obj);
        if (tmp) { s->estimated_tokens += util_estimate_tokens(tmp); free(tmp); }
        cJSON_AddItemToArray(s->messages, obj);
        // if no system prompt exists and this is first load, keep as is
    }
    fclose(f);
    // If file had no messages (empty), add system prompt
    if (cJSON_GetArraySize(s->messages)==0) {
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", openai_system_prompt());
        cJSON_AddItemToArray(s->messages, sys);
    }
    return true;
}

bool session_append(Session *s, cJSON *msg) {
    if (!s || !msg) return false;
    cJSON *dup = cJSON_Duplicate(msg, 1);
    char *tmp = cJSON_PrintUnformatted(dup);
    if (tmp) { s->estimated_tokens += util_estimate_tokens(tmp); free(tmp); }
    cJSON_AddItemToArray(s->messages, dup);
    // track file touches if msg contains tool results referencing files
    // Do incremental compaction check
    session_maybe_compact(s);
    return true;
}

void session_add_touched(Session *s, const char *path) {
    if (!s || !path) return;
    for (int i=0;i<s->touched_count;i++) if (strcmp(s->touched_files[i], path)==0) return;
    if (s->touched_count<256) s->touched_files[s->touched_count++]=strdup(path);
}

void session_update_summary(Session *s, const char *new_content) {
    if (!s || !new_content) return;
    // Incremental merge: append file list + delta, not re-read full history (Lever #3)
    // Simple heuristic: summary = existing summary + "\n- " + new_content truncated
    size_t cur_len = strlen(s->summary);
    size_t add_len = strlen(new_content);
    if (cur_len + add_len + 4 < sizeof(s->summary)) {
        if (cur_len>0) strcat(s->summary, "\n");
        strcat(s->summary, new_content);
    } else {
        // truncate oldest part
        size_t keep = sizeof(s->summary) - add_len - 8;
        if (keep < 512) keep=512;
        memmove(s->summary, s->summary + cur_len - keep, keep);
        s->summary[keep]='\0';
        strcat(s->summary, "\n");
        strncat(s->summary, new_content, sizeof(s->summary)-strlen(s->summary)-1);
    }
}

void session_maybe_compact(Session *s) {
    if (!s || !s->messages) return;
    if (s->estimated_tokens < COMPACT_WINDOW_TOKENS) return;
    // Lever #3: delete instead of summarizing for abandoned, and incremental summary
    int n = cJSON_GetArraySize(s->messages);
    if (n < 4) return;
    // Keep system (index 0) + recent window
    size_t recent_tokens=0;
    int keep_from = n-1;
    for (int i=n-1; i>=1; i--) {
        cJSON *msg = cJSON_GetArrayItem(s->messages, i);
        char *tmp = cJSON_PrintUnformatted(msg);
        size_t toks = tmp ? util_estimate_tokens(tmp) : 0;
        if (tmp) free(tmp);
        recent_tokens += toks;
        if (recent_tokens >= COMPACT_KEEP_RECENT) { keep_from=i; break; }
        if (i==1) keep_from=1;
    }
    if (keep_from <= 1) return;
    // Build summary incrementally from messages to be dropped (1 .. keep_from-1)
    // Record file paths, not contents ( ~1 token per filename )
    char delta[2048]="";
    snprintf(delta, sizeof(delta), "Compacted %d messages. Files touched:", keep_from-1);
    for (int i=1;i<keep_from;i++) {
        // try to extract file touches from tool calls?
        // For now just note count
    }
    if (s->touched_count>0) {
        strncat(delta, " ", sizeof(delta)-strlen(delta)-1);
        for (int i=0;i<s->touched_count && strlen(delta)<1800;i++) {
            strncat(delta, s->touched_files[i], sizeof(delta)-strlen(delta)-1);
            strncat(delta, " ", sizeof(delta)-strlen(delta)-1);
        }
    }
    session_update_summary(s, delta);

    // Create new array: system + summary message + recent
    cJSON *new_arr = cJSON_CreateArray();
    cJSON *sys = cJSON_GetArrayItem(s->messages, 0);
    cJSON_AddItemToArray(new_arr, cJSON_Duplicate(sys, 1));
    if (s->summary[0]) {
        cJSON *sum_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(sum_msg, "role", "system");
        char sum_content[SESSION_SUMMARY_MAX+64];
        snprintf(sum_content, sizeof(sum_content), "[Session summary so far: %s]", s->summary);
        cJSON_AddStringToObject(sum_msg, "content", sum_content);
        cJSON_AddItemToArray(new_arr, sum_msg);
    }
    for (int i=keep_from;i<n;i++) {
        cJSON_AddItemToArray(new_arr, cJSON_Duplicate(cJSON_GetArrayItem(s->messages, i), 1));
    }
    cJSON_Delete(s->messages);
    s->messages = new_arr;
    // recalc tokens
    s->estimated_tokens=0;
    int nn = cJSON_GetArraySize(s->messages);
    for (int i=0;i<nn;i++) {
        char *tmp = cJSON_PrintUnformatted(cJSON_GetArrayItem(s->messages,i));
        if (tmp) { s->estimated_tokens += util_estimate_tokens(tmp); free(tmp); }
    }
}

bool session_branch(Session *s, const char *branch_id) {
    (void)s; (void)branch_id;
    // TODO: tree branch handling - for now just create new id
    return true;
}
void session_delete_branch(const char *branch_id) {
    if (!branch_id) return;
    char path[XZERO_PATH_MAX];
    session_get_path(branch_id, path, sizeof(path));
    remove(path);
    // Lever #3: abandoned branches are deleted entirely, not summarized - so we remove file
}

cJSON *session_messages_for_api(Session *s) {
    if (!s || !s->messages) return cJSON_CreateArray();
    // Return messages as-is; caller will feed to openai_build_payload which ensures cache ordering
    // Ensure we don't leak internal meta
    // Copy array
    return cJSON_Duplicate(s->messages, 1);
}

void session_list(char ids[][SESSION_ID_MAX], int *count, int max) {
    if (!count) return;
    *count=0;
    char dir[XZERO_PATH_MAX];
    session_get_dir(dir, sizeof(dir));
#ifdef _WIN32
    char pattern[XZERO_PATH_MAX];
    util_path_join(pattern, sizeof(pattern), dir, "*.jsonl");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h==INVALID_HANDLE_VALUE) return;
    do {
        if (*count>=max) break;
        char *dot = strrchr(fd.cFileName, '.');
        if (dot) *dot='\0';
        util_str_copy(ids[*count], SESSION_ID_MAX, fd.cFileName);
        (*count)++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent=readdir(d)) && *count < max) {
        if (strstr(ent->d_name, ".jsonl")) {
            char *dot=strrchr(ent->d_name,'.');
            if(dot) *dot='\0';
            util_str_copy(ids[*count], SESSION_ID_MAX, ent->d_name);
            (*count)++;
        }
    }
    closedir(d);
#endif
}
