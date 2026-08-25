#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <cJSON.h>
#include "util.h"
#include "config.h"

#define SESSION_ID_MAX 64
#define SESSION_SUMMARY_MAX 4096

typedef struct Session {
    char id[SESSION_ID_MAX];
    char model[XZERO_MODEL_MAX];
    cJSON *messages; // array of message objects
    char summary[SESSION_SUMMARY_MAX]; // incremental summary (Lever #3)
    char *touched_files[256];
    int touched_count;
    size_t estimated_tokens; // for compaction heuristic
    char file_path[XZERO_PATH_MAX]; // path to session file
} Session;

Session *session_create(const char *model);
void session_free(Session *s);
bool session_load(Session *s, const char *id_or_path);
bool session_save(Session *s);
bool session_append(Session *s, cJSON *msg); // takes ownership (duplicates)
void session_add_touched(Session *s, const char *path);

// Lever #3: compaction - keep 20k tokens window, summarize rest incrementally
void session_maybe_compact(Session *s);
void session_update_summary(Session *s, const char *new_content);

// Branch handling: tree of sessions - delete abandoned branch
bool session_branch(Session *s, const char *branch_id);
void session_delete_branch(const char *branch_id);

// Helpers
void session_get_dir(char *out, size_t n);
void session_get_path(const char *id, char *out, size_t n);
cJSON *session_messages_for_api(Session *s); // returns array ready for payload (includes summary if compacted)
void session_list(char ids[][SESSION_ID_MAX], int *count, int max);
