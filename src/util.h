#pragma once
#include <stddef.h>
#include <stdbool.h>

#define XZERO_PATH_MAX 1024
#define XZERO_URL_MAX 512
#define XZERO_KEY_MAX 1024
#define XZERO_MODEL_MAX 128

// URL normalization: trim whitespace, remove trailing '/', handle duplicate /v1
void util_url_normalize(const char *input, char *out, size_t out_n);

// Trim whitespace in place
void util_trim(char *s);

// Join path components (handles / vs \)
void util_path_join(char *out, size_t out_n, const char *a, const char *b);

// Ensure directory exists (mkdir -p)
bool util_mkdir_p(const char *path);

// Estimate tokens ~ chars/4 (for compaction heuristics)
size_t util_estimate_tokens(const char *s);
size_t util_estimate_tokens_n(const char *s, size_t n);

// Safe str copy
void util_str_copy(char *dst, size_t n, const char *src);

// Get temp dir
void util_temp_dir(char *out, size_t n);

// Generate random hex id (time+pid+rand)
void util_generate_id(char *out, size_t n);

// Check if path is within cwd (prevent directory escape)
bool util_path_is_within_cwd(const char *path);

// Truncate helpers for Lever #1: cap at 50KB / 2000 lines, return tail
// Returns malloc'd string that caller must free. Sets *truncated = true if capped.
// If truncated, output is last 50KB/2000 lines + footer with full_path hint.
char *util_cap_output(const char *full_output, size_t full_len, const char *full_path_hint, bool *truncated, size_t *out_lines);

// Human vs model split: write full to file, return receipt
// Writes full_output to spill file if truncated, returns receipt string for model
char *util_spill_and_receipt(const char *full_output, size_t full_len, const char *spill_path);

size_t util_count_lines(const char *s, size_t n);
