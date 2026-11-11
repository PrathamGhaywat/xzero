#pragma once
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    long status;
    char *body;
    size_t body_len;
    bool truncated;
    char *headers;
} HttpResponse;

void http_response_free(HttpResponse *r);

// Simple GET/POST (non-streaming)
int http_get(const char *url, const char * const *headers, HttpResponse *out);
int http_post_json(const char *url, const char *json_body, const char * const *headers, HttpResponse *out);

// Streaming: callback per chunk
typedef int (*http_stream_cb)(const char *chunk, size_t len, void *user);

// Streaming POST. `out` receives the HTTP status and, for non-SSE responses
// (including HTTP errors), the raw response body. The body is not retained for
// successful SSE responses. Caller must call http_response_free(out).
int http_post_stream(const char *url, const char *json_body, const char * const *headers, http_stream_cb cb, void *user, HttpResponse *out);

void http_global_init(void);
void http_global_cleanup(void);
