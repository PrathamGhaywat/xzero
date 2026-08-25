#include "stream.h"
#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>

void stream_result_init(StreamResult *r) {
    if (!r) return;
    memset(r, 0, sizeof(*r));
    r->tool_calls = cJSON_CreateArray();
}
void stream_result_free(StreamResult *r) {
    if (!r) return;
    free(r->content_delta);
    if (r->tool_calls) cJSON_Delete(r->tool_calls);
    free(r->finish_reason);
    r->content_delta=NULL;
    r->tool_calls=NULL;
}

void stream_state_init(StreamState *s, StreamResult *r, bool print) {
    memset(s, 0, sizeof(*s));
    s->result = r;
    s->print_to_stdout = print;
    s->buf_cap = 8192;
    s->buffer = (char*)malloc(s->buf_cap);
    s->buf_len=0;
}
void stream_state_free(StreamState *s) {
    free(s->buffer);
    s->buffer=NULL;
}

static void append_content(StreamResult *r, const char *delta) {
    if (!delta || !delta[0]) return;
    size_t dl = strlen(delta);
    size_t nl = r->content_len + dl + 1;
    char *newp = (char*)realloc(r->content_delta, nl);
    if (!newp) return;
    r->content_delta = newp;
    memcpy(r->content_delta + r->content_len, delta, dl);
    r->content_len += dl;
    r->content_delta[r->content_len]='\0';
}

static void handle_json_object(StreamState *s, cJSON *root) {
    if (!root) return;
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices)==0) {
        // check usage
        cJSON *usage = cJSON_GetObjectItem(root, "usage");
        if (usage) {
            cJSON *pt = cJSON_GetObjectItem(usage, "prompt_tokens");
            cJSON *ct = cJSON_GetObjectItem(usage, "completion_tokens");
            if (cJSON_IsNumber(pt)) s->result->prompt_tokens = pt->valueint;
            if (cJSON_IsNumber(ct)) s->result->completion_tokens = ct->valueint;
        }
        return;
    }
    cJSON *ch = cJSON_GetArrayItem(choices, 0);
    cJSON *delta = cJSON_GetObjectItem(ch, "delta");
    cJSON *finish = cJSON_GetObjectItem(ch, "finish_reason");
    if (!delta) delta = cJSON_GetObjectItem(ch, "message"); // non-stream fallback

    if (delta) {
        cJSON *content = cJSON_GetObjectItem(delta, "content");
        if (cJSON_IsString(content) && content->valuestring) {
            append_content(s->result, content->valuestring);
            if (s->print_to_stdout) {
                fputs(content->valuestring, stdout);
                fflush(stdout);
            }
        }
        cJSON *tool_calls = cJSON_GetObjectItem(delta, "tool_calls");
        if (cJSON_IsArray(tool_calls)) {
            int n = cJSON_GetArraySize(tool_calls);
            for (int i=0;i<n;i++) {
                cJSON *tc = cJSON_GetArrayItem(tool_calls, i);
                cJSON *idx = cJSON_GetObjectItem(tc, "index");
                int index = cJSON_IsNumber(idx) ? idx->valueint : 0;
                // ensure array size
                while (cJSON_GetArraySize(s->result->tool_calls) <= index) {
                    cJSON *placeholder = cJSON_CreateObject();
                    cJSON_AddStringToObject(placeholder, "id", "");
                    cJSON_AddStringToObject(placeholder, "name", "");
                    cJSON_AddStringToObject(placeholder, "arguments", "");
                    // also store type
                    cJSON_AddItemToArray(s->result->tool_calls, placeholder);
                }
                cJSON *existing = cJSON_GetArrayItem(s->result->tool_calls, index);
                cJSON *id = cJSON_GetObjectItem(tc, "id");
                cJSON *func = cJSON_GetObjectItem(tc, "function");
                if (cJSON_IsString(id) && id->valuestring[0]) {
                    cJSON_ReplaceItemInObject(existing, "id", cJSON_CreateString(id->valuestring));
                }
                if (func) {
                    cJSON *name = cJSON_GetObjectItem(func, "name");
                    cJSON *args = cJSON_GetObjectItem(func, "arguments");
                    if (cJSON_IsString(name) && name->valuestring[0]) {
                        // append name if existing non-empty? but name usually comes once
                        cJSON *cur = cJSON_GetObjectItem(existing, "name");
                        if (cur && cJSON_IsString(cur) && cur->valuestring[0]==0) {
                            cJSON_ReplaceItemInObject(existing, "name", cJSON_CreateString(name->valuestring));
                        } else if (!cur || !cJSON_IsString(cur)) {
                            cJSON_ReplaceItemInObject(existing, "name", cJSON_CreateString(name->valuestring));
                        }
                    }
                    if (cJSON_IsString(args) && args->valuestring) {
                        cJSON *cur = cJSON_GetObjectItem(existing, "arguments");
                        const char *prev = (cur && cJSON_IsString(cur)) ? cur->valuestring : "";
                        size_t newlen = strlen(prev) + strlen(args->valuestring) + 1;
                        char *combined = (char*)malloc(newlen);
                        if (combined) {
                            strcpy(combined, prev);
                            strcat(combined, args->valuestring);
                            cJSON_ReplaceItemInObject(existing, "arguments", cJSON_CreateString(combined));
                            free(combined);
                        }
                    }
                }
            }
        }
    }
    if (cJSON_IsString(finish) && finish->valuestring) {
        free(s->result->finish_reason);
        s->result->finish_reason = strdup(finish->valuestring);
        if (strcmp(finish->valuestring, "tool_calls")==0) {
            // tool calls finished
        }
        if (strcmp(finish->valuestring, "stop")!=0 && strcmp(finish->valuestring, "tool_calls")!=0) {
            // other
        }
    }
}

static void process_line(StreamState *s, const char *line, size_t len) {
    // trim
    while (len>0 && (line[len-1]=='\r' || line[len-1]=='\n')) len--;
    size_t start=0;
    while (start<len && (line[start]==' ' || line[start]=='\t')) start++;
    if (start>=len) return;
    const char *p = line+start;
    size_t plen = len - start;
    if (plen >= 5 && strncmp(p, "data:", 5)==0) {
        p+=5;
        plen-=5;
        while (plen>0 && (*p==' ' || *p=='\t')) { p++; plen--; }
        if (plen==0) return;
        if (plen==6 && strncmp(p, "[DONE]", 6)==0) {
            s->result->done=true;
            return;
        }
        // copy to null-terminated
        char *tmp = (char*)malloc(plen+1);
        if (!tmp) return;
        memcpy(tmp, p, plen);
        tmp[plen]='\0';
        cJSON *json = cJSON_Parse(tmp);
        free(tmp);
        if (json) {
            handle_json_object(s, json);
            cJSON_Delete(json);
        }
    }
}

int stream_sse_callback(const char *chunk, size_t len, void *user) {
    StreamState *s = (StreamState*)user;
    if (!s || !chunk || len==0) return 1;
    // Append to buffer and process complete lines
    size_t needed = s->buf_len + len + 1;
    if (needed > s->buf_cap) {
        size_t newcap = s->buf_cap*2;
        while (newcap < needed) newcap*=2;
        char *nb = (char*)realloc(s->buffer, newcap);
        if (!nb) return 0;
        s->buffer=nb;
        s->buf_cap=newcap;
    }
    memcpy(s->buffer + s->buf_len, chunk, len);
    s->buf_len += len;
    s->buffer[s->buf_len]='\0';
    // process lines up to last \n
    char *last_nl = strrchr(s->buffer, '\n');
    if (!last_nl) return 1;
    size_t process_len = last_nl - s->buffer + 1;
    // Iterate lines
    char *ptr = s->buffer;
    while (ptr < s->buffer + process_len) {
        char *nl = strchr(ptr, '\n');
        if (!nl) break;
        size_t ll = nl - ptr + 1;
        process_line(s, ptr, ll);
        ptr = nl+1;
    }
    // move remaining
    size_t remaining = s->buf_len - process_len;
    memmove(s->buffer, s->buffer+process_len, remaining);
    s->buf_len = remaining;
    s->buffer[s->buf_len]='\0';
    return 1;
}

int stream_chat(const char *url, const char *json_body, const char * const *headers, StreamResult *result) {
    if (!url || !json_body || !result) return -1;
    StreamState state;
    stream_state_init(&state, result, true);
    int rc = http_post_stream(url, json_body, headers, stream_sse_callback, &state);
    // flush any remaining buffer as line (if no trailing \n)
    if (state.buf_len>0) {
        process_line(&state, state.buffer, state.buf_len);
    }
    // If no streaming detected (e.g., server returned non-stream JSON), try parse as whole JSON
    if (!result->content_delta && !result->done && cJSON_GetArraySize(result->tool_calls)==0) {
        // The callback may not have been invoked as SSE - try direct parse
        // This can happen if server ignores stream:true
        // We treat the buffered data as possible JSON
        if (state.buf_len>0 || state.buffer) {
            // Not needed - http_post_stream already handled
        }
    }
    // Clean up empty tool calls (those with no name)
    if (result->tool_calls) {
        // Remove placeholders with empty name and id
        int sz = cJSON_GetArraySize(result->tool_calls);
        // We'll keep but caller can check name empty
        // Compact: if all empty, delete array and set to empty
        bool any=false;
        for (int i=0;i<sz;i++) {
            cJSON *tc = cJSON_GetArrayItem(result->tool_calls, i);
            cJSON *name = cJSON_GetObjectItem(tc, "name");
            if (name && cJSON_IsString(name) && name->valuestring[0]) { any=true; break; }
        }
        if (!any) {
            cJSON_Delete(result->tool_calls);
            result->tool_calls = cJSON_CreateArray();
        }
    }
    stream_state_free(&state);
    return rc;
}
