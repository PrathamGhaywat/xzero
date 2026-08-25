#pragma once
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    long status;
    char *body;
    size_t body_len;
    char *headers;
} HttpResponse;

void http_response_free(HttpResponse *r);

// Simple GET/POST (non-streaming)
int http_get(const char *url, const char * const *headers, HttpResponse *out);
int http_post_json(const char *url, const char *json_body, const char * const *headers, HttpResponse *out);

// Streaming: callback per chunk
typedef int (*http_stream_cb)(const char *chunk, size_t len, void *user);

int http_post_stream(const char *url, const char *json_body, const char * const *headers, http_stream_cb cb, void *user);

void http_global_init(void);
void http_global_cleanup(void);
