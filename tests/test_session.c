#undef NDEBUG
#include "../src/session.h"
#include "../src/util.h"
#include <cJSON.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The real implementation lives in openai.c, which would drag in libcurl.
const char *openai_system_prompt(void) {
    return "test system prompt";
}

static void add_message(Session *s, const char *role, const char *content) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", role);
    cJSON_AddStringToObject(m, "content", content);
    assert(session_append(s, m));
    cJSON_Delete(m);
}

static void add_assistant_tool_call(Session *s, const char *id, const char *name) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "assistant");
    cJSON_AddStringToObject(m, "content", "");
    cJSON *arr = cJSON_CreateArray();
    cJSON *tc = cJSON_CreateObject();
    cJSON_AddStringToObject(tc, "id", id);
    cJSON_AddStringToObject(tc, "type", "function");
    cJSON *fn = cJSON_CreateObject();
    cJSON_AddStringToObject(fn, "name", name);
    cJSON_AddStringToObject(fn, "arguments", "{}");
    cJSON_AddItemToObject(tc, "function", fn);
    cJSON_AddItemToArray(arr, tc);
    cJSON_AddItemToObject(m, "tool_calls", arr);
    assert(session_append(s, m));
    cJSON_Delete(m);
}

static void add_tool_result(Session *s, const char *id, const char *content) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "tool");
    cJSON_AddStringToObject(m, "tool_call_id", id);
    cJSON_AddStringToObject(m, "content", content);
    assert(session_append(s, m));
    cJSON_Delete(m);
}

static void free_session(Session *s) {
    if (s->messages) cJSON_Delete(s->messages);
    for (int i=0;i<s->touched_count;i++) free(s->touched_files[i]);
    memset(s, 0, sizeof(*s));
}

static void make_session(Session *s, char *path, size_t path_n) {
    memset(s, 0, sizeof(*s));
    util_generate_id(s->id, sizeof(s->id));
    util_str_copy(s->model, sizeof(s->model), "test-model");
    s->messages = cJSON_CreateArray();
    char dir[XZERO_PATH_MAX];
    char fname[256];
    util_temp_dir(dir, sizeof(dir));
    snprintf(fname, sizeof(fname), "xzero_test_session_%s.jsonl", s->id);
    util_path_join(path, path_n, dir, fname);
    util_str_copy(s->file_path, sizeof(s->file_path), path);
}

static void test_long_line_roundtrip(void) {
    Session s;
    char path[XZERO_PATH_MAX];
    make_session(&s, path, sizeof(path));
    add_message(&s, "system", "sys");
    size_t big_len = 200*1024;
    char *big = (char*)malloc(big_len+1);
    assert(big);
    memset(big, 'a', big_len);
    big[big_len]='\0';
    add_message(&s, "user", big);
    free(big);

    assert(session_save(&s));

    Session loaded;
    memset(&loaded, 0, sizeof(loaded));
    assert(session_load(&loaded, path));
    assert(cJSON_GetArraySize(loaded.messages)==2);
    cJSON *m = cJSON_GetArrayItem(loaded.messages, 1);
    cJSON *content = cJSON_GetObjectItem(m, "content");
    assert(cJSON_IsString(content));
    assert(strlen(content->valuestring)==big_len);

    free_session(&loaded);
    free_session(&s);
    remove(path);
    printf("test_session long-line roundtrip passed\n");
}

static void test_compaction_keeps_tool_pairs(void) {
    Session s;
    char path[XZERO_PATH_MAX];
    make_session(&s, path, sizeof(path));
    add_message(&s, "system", "sys");
    add_message(&s, "user", "old request");
    add_assistant_tool_call(&s, "call_1", "read");
    char *big = (char*)malloc(40000);
    assert(big);
    memset(big, 'x', 39999);
    big[39999]='\0';
    add_tool_result(&s, "call_1", big);
    free(big);
    add_message(&s, "user", "latest request");

    // Force compaction. The retained window would otherwise start at the
    // standalone tool message, orphaning it from its assistant tool_calls.
    s.estimated_tokens = 100000;
    session_compact(&s, true);

    int n = cJSON_GetArraySize(s.messages);
    bool ever_tool_call = false;
    bool current_tool_call = false;
    bool saw_tool_result = false;
    for (int i=0;i<n;i++) {
        cJSON *m = cJSON_GetArrayItem(s.messages, i);
        cJSON *role = cJSON_GetObjectItem(m, "role");
        if (!cJSON_IsString(role)) continue;
        if (strcmp(role->valuestring, "assistant")==0) {
            cJSON *tcs = cJSON_GetObjectItem(m, "tool_calls");
            current_tool_call = cJSON_IsArray(tcs) && cJSON_GetArraySize(tcs)>0;
            if (current_tool_call) ever_tool_call = true;
        } else if (strcmp(role->valuestring, "tool")==0) {
            saw_tool_result = true;
            assert(current_tool_call); // never start with an orphaned tool result
        } else {
            current_tool_call = false;
        }
    }
    assert(ever_tool_call); // the assistant tool_calls message must be kept
    assert(saw_tool_result);

    free_session(&s);
    remove(path);
    printf("test_session compaction boundary passed\n");
}

int main(void) {
    test_long_line_roundtrip();
    test_compaction_keeps_tool_pairs();
    printf("test_session passed\n");
    return 0;
}
