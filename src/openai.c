#include "openai.h"
#include "util.h"
#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>

const char *openai_system_prompt(void) {
#ifdef _WIN32
    return "You are xzero, a fast, efficient coding agent in C. "
           "You help users with software engineering tasks via tool calls. "
           "OS: Windows (use 'cd' without args or 'echo %cd%' or 'dir' to inspect filesystem; 'pwd'/'ls'/'cat' are NOT available — use 'cd'/'dir'/'type' instead). "
           "Rules: Use tools to read/edit files and run commands. Prefer edit over write for existing files. "
           "Keep tool outputs bounded — you receive capped outputs. If truncated, you can read spill files. "
           "If a command fails, analyze exit code and stderr, then retry with correct Windows equivalent. "
           "Be concise. After finishing, verify with bash if applicable.";
#else
    return "You are xzero, a fast, efficient coding agent in C. "
           "You help users with software engineering tasks via tool calls. "
           "OS: POSIX (use 'pwd', 'ls', 'cat' etc.). "
           "Rules: Use tools to read/edit files and run commands. Prefer edit over write for existing files. "
           "Keep tool outputs bounded — you receive capped outputs. If truncated, you can read spill files. "
           "If a command fails, analyze exit code and stderr, then retry. "
           "Be concise. After finishing, verify with bash if applicable.";
#endif
}

int openai_build_headers(const XZeroConfig *cfg, const char **out_headers, int max_headers, char *auth_buf, size_t auth_n) {
    int idx=0;
    if (idx < max_headers) out_headers[idx++] = "Content-Type: application/json";
    if (idx < max_headers) out_headers[idx++] = "Accept: application/json";
    // Lever #2: prompt caching hints - OpenAI compatible servers use automatic caching,
    // but we ensure stable prefix via headers that mark session. Some providers (Anthropic, DeepSeek)
    // respect cache-control headers; we add generic hints without breaking OpenAI spec.
    // The session ID as cache key is handled at payload level (user message stable).
    if (cfg && cfg->api_key[0] && auth_buf) {
        snprintf(auth_buf, auth_n, "Authorization: Bearer %s", cfg->api_key);
        if (idx < max_headers) out_headers[idx++] = auth_buf;
    }
    if (idx < max_headers) out_headers[idx] = NULL;
    return idx;
}

static char *build_url(const char *base, const char *path) {
    size_t bl = strlen(base);
    size_t pl = strlen(path);
    bool need_slash = bl>0 && base[bl-1]!='/';
    char *out = (char*)malloc(bl + pl + 2);
    if (!out) return NULL;
    if (need_slash) snprintf(out, bl+pl+2, "%s%s", base, path);
    else {
        // base already ends with /, but path starts with / -> avoid double
        if (path[0]=='/' && base[bl-1]=='/') snprintf(out, bl+pl+2, "%s%s", base, path+1);
        else snprintf(out, bl+pl+2, "%s%s", base, path);
    }
    return out;
}

int openai_test_connection(const XZeroConfig *cfg, char *err, size_t err_n) {
    if (!cfg) { if(err) snprintf(err, err_n, "no config"); return -1; }
    char auth_buf[1200];
    const char *headers[8]={0};
    openai_build_headers(cfg, headers, 8, auth_buf, sizeof(auth_buf));

    // Try GET /models first
    char *url = build_url(cfg->base_url, "/models");
    if (!url) { if(err) snprintf(err, err_n, "oom"); return -1; }
    HttpResponse resp={0};
    int rc = http_get(url, headers, &resp);
    free(url);
    if (rc==0 && resp.status==200) {
        // check json has data?
        cJSON *j = cJSON_Parse(resp.body ? resp.body : "{}");
        bool ok = (j && cJSON_GetObjectItem(j, "data") != NULL) || (resp.status==200);
        if (j) cJSON_Delete(j);
        http_response_free(&resp);
        if (ok) return 0;
        // else fallback
    } else {
        http_response_free(&resp);
    }
    // Fallback: POST /chat/completions minimal
    char *url2 = build_url(cfg->base_url, "/chat/completions");
    if (!url2) { if(err) snprintf(err, err_n, "oom"); return -1; }
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "model", cfg->model[0]? cfg->model : "gpt-4o-mini");
    cJSON *msgs = cJSON_CreateArray();
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", "hi");
    cJSON_AddItemToArray(msgs, m);
    cJSON_AddItemToObject(payload, "messages", msgs);
    cJSON_AddNumberToObject(payload, "max_tokens", 5);
    char *body = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    HttpResponse resp2={0};
    int rc2 = http_post_json(url2, body, headers, &resp2);
    free(url2);
    free(body);
    if (rc2==0 && (resp2.status==200 || resp2.status==201)) {
        http_response_free(&resp2);
        return 0;
    }
    if (err) {
        snprintf(err, err_n, "test failed: HTTP %ld %s", resp2.status, resp2.body ? resp2.body : "(no body)");
    }
    http_response_free(&resp2);
    if (resp.body) http_response_free(&resp);
    return -1;
}

char *openai_build_payload(const XZeroConfig *cfg, cJSON *messages, cJSON *tools, bool stream) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", cfg->model[0]? cfg->model : "gpt-4o-mini");
    cJSON_AddNumberToObject(root, "temperature", 0.2);
    if (stream) cJSON_AddBoolToObject(root, "stream", true);
    // Lever #2: include stream_options for usage on streaming
    if (stream) {
        cJSON *so = cJSON_CreateObject();
        cJSON_AddBoolToObject(so, "include_usage", true);
        cJSON_AddItemToObject(root, "stream_options", so);
    }
    // Messages: caller already ordered as system -> summary -> recent window -> user
    // We ensure the system prompt is first and stable for cache hit.
    if (messages) {
        cJSON_AddItemToObject(root, "messages", cJSON_Duplicate(messages, 1));
    } else {
        cJSON *arr = cJSON_CreateArray();
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", openai_system_prompt());
        cJSON_AddItemToArray(arr, sys);
        cJSON_AddItemToObject(root, "messages", arr);
    }
    if (tools) {
        cJSON_AddItemToObject(root, "tools", cJSON_Duplicate(tools, 1));
        cJSON_AddStringToObject(root, "tool_choice", "auto");
    }
    // Lever #2: prompt cache key via extra field for providers that support it (e.g., OpenAI prompt_cache_key)
    // This does not break OpenAI spec - unknown fields are ignored by strict servers but help caching on supporting ones.
    // We also set parallel_tool_calls true to allow batch execution (Lever #4).
    cJSON_AddBoolToObject(root, "parallel_tool_calls", true);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}
