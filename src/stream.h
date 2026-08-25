#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <cJSON.h>

typedef struct {
    char *content_delta; // accumulated
    size_t content_len;
    // tool calls incremental
    cJSON *tool_calls; // array of {id, name, arguments}
    char *finish_reason;
    bool done;
    // usage
    int prompt_tokens;
    int completion_tokens;
} StreamResult;

void stream_result_init(StreamResult *r);
void stream_result_free(StreamResult *r);

// Callback for http_post_stream: parse SSE lines: data: {...}
int stream_sse_callback(const char *chunk, size_t len, void *user);

// Helper to run full streaming request and block until done
// Returns 0 on success, -1 on error. Fills result.
int stream_chat(const char *url, const char *json_body, const char * const *headers, StreamResult *result);

// Internal state for callback
typedef struct {
    char *buffer; // incomplete line buffer
    size_t buf_len;
    size_t buf_cap;
    StreamResult *result;
    // for incremental print: if true, print content delta to stdout as it arrives (human receives rich)
    bool print_to_stdout;
} StreamState;

void stream_state_init(StreamState *s, StreamResult *r, bool print);
void stream_state_free(StreamState *s);
