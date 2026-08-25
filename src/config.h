#pragma once
#include "util.h"
#include <stdbool.h>

typedef struct {
    char base_url[XZERO_URL_MAX];
    char api_key[XZERO_KEY_MAX];
    char model[XZERO_MODEL_MAX];
} XZeroConfig;

void config_init_defaults(XZeroConfig *cfg);
bool config_load(XZeroConfig *cfg, const char *override_path);
bool config_save(const XZeroConfig *cfg, const char *override_path);
void config_get_global_path(char *out, size_t n);
void config_get_local_path(char *out, size_t n);
bool config_exists(const char *path);
void config_mask_key(const char *key, char *out, size_t n);
bool config_validate(const XZeroConfig *cfg, char *err, size_t err_n);
void config_apply_env_overrides(XZeroConfig *cfg);
