#pragma once
#include "config.h"
#include "http.h"
#include <stdbool.h>
#include <cJSON.h>

// Build headers for OpenAI request (Authorization only if key non-empty)
int openai_build_headers(const XZeroConfig *cfg, const char **out_headers, int max_headers, char *auth_buf, size_t auth_n);

// Test connection: GET /models or POST /chat/completions fallback
int openai_test_connection(const XZeroConfig *cfg, char *err, size_t err_n);

// Build chat completions payload with prompt-cache-aware ordering
// Lever #2: stable prefix for caching: system -> tools -> history -> last user
// Returns malloc'd JSON string (free with free())
char *openai_build_payload(const XZeroConfig *cfg, cJSON *messages, cJSON *tools, bool stream);

// System prompt (cache breakpoint 1)
const char *openai_system_prompt(void);
