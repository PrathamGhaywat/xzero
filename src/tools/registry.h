#pragma once
#include <cJSON.h>
#include <stdbool.h>
#include "session.h"

#define TOOL_OUTPUT_MAX (64*1024)

typedef struct {
    char *content; // malloc'd, bounded for model (Lever #1)
    char *full_path; // spill path for human if truncated (NULL if not)
    bool is_error;
    bool truncated;
} ToolResult;

void tool_result_free(ToolResult *r);

// Registry
typedef ToolResult (*tool_fn)(cJSON *args, Session *session);

typedef struct {
    const char *name;
    const char *description;
    tool_fn fn;
    cJSON *parameters_schema; // owned
} ToolDef;

int tools_count(void);
const ToolDef *tools_get(int idx);
const ToolDef *tools_find(const char *name);

// Execute tool by name (parses args string if needed)
ToolResult tools_dispatch(const char *name, const char *args_json_str, Session *session);

// Build OpenAI tools JSON array (Lever #2: stable ordering for cache)
cJSON *tools_build_openai_array(void);

// Approval check (write/edit/bash need confirm)
bool tools_needs_approval(const char *name);
