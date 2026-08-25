#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <unistd.h>
#include <pwd.h>
#endif

#include <cJSON.h>

void config_init_defaults(XZeroConfig *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    util_str_copy(cfg->base_url, sizeof(cfg->base_url), "https://api.openai.com/v1");
    util_str_copy(cfg->model, sizeof(cfg->model), "gpt-4o-mini");
    cfg->api_key[0]='\0';
}

void config_get_global_path(char *out, size_t n) {
#ifdef _WIN32
    char *appdata = getenv("APPDATA");
    if (appdata && appdata[0]) {
        util_path_join(out, n, appdata, "xzero");
        size_t len = strlen(out);
        util_path_join(out, n, out, "config.json");
        return;
    }
    // fallback to USERPROFILE
    char *userprofile = getenv("USERPROFILE");
    if (userprofile) {
        util_path_join(out, n, userprofile, ".xzero");
        util_path_join(out, n, out, "config.json");
        return;
    }
    util_str_copy(out, n, "xzero_config.json");
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) {
        util_path_join(out, n, xdg, "xzero/config.json");
        return;
    }
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    // macOS prefers Library/Application Support but also support XDG
#ifdef __APPLE__
    if (home) {
        char tmp[XZERO_PATH_MAX];
        util_path_join(tmp, sizeof(tmp), home, "Library/Application Support/xzero/config.json");
        // check if exists, otherwise use XDG
        struct stat st;
        if (stat(tmp, &st)==0) {
            util_str_copy(out, n, tmp);
            return;
        }
    }
#endif
    if (home) {
        util_path_join(out, n, home, ".config/xzero/config.json");
        return;
    }
    util_str_copy(out, n, "./xzero.json");
#endif
}

void config_get_local_path(char *out, size_t n) {
    // Check ./xzero.json first, then ./.xzero/config.json
    struct stat st;
    if (stat("./xzero.json", &st)==0) {
        util_str_copy(out, n, "./xzero.json");
        return;
    }
    if (stat("./.xzero/config.json", &st)==0) {
        util_str_copy(out, n, "./.xzero/config.json");
        return;
    }
    // default local not exists -> return ./xzero.json as candidate
    util_str_copy(out, n, "./xzero.json");
}

bool config_exists(const char *path) {
    if (!path) return false;
    struct stat st;
    return stat(path, &st)==0;
}

void config_mask_key(const char *key, char *out, size_t n) {
    if (!key || !out || n==0) return;
    size_t len = strlen(key);
    if (len==0) { util_str_copy(out, n, "(none)"); return; }
    if (len <= 8) { snprintf(out, n, "%s****", key); return; }
    char prefix[16]={0};
    strncpy(prefix, key, 7);
    prefix[7]='\0';
    snprintf(out, n, "%s...**** (%zu chars)", prefix, len);
}

bool config_validate(const XZeroConfig *cfg, char *err, size_t err_n) {
    if (!cfg) {
        if (err) snprintf(err, err_n, "config is null");
        return false;
    }
    if (!cfg->base_url[0]) {
        if (err) snprintf(err, err_n, "base_url is required");
        return false;
    }
    if (strncmp(cfg->base_url, "http://", 7)!=0 && strncmp(cfg->base_url, "https://", 8)!=0) {
        if (err) snprintf(err, err_n, "base_url must start with http:// or https://");
        return false;
    }
    // model can be empty but warn
    return true;
}

void config_apply_env_overrides(XZeroConfig *cfg) {
    if (!cfg) return;
    const char *base = getenv("XZERO_BASE_URL");
    if (!base) base = getenv("OPENAI_BASE_URL");
    if (!base) base = getenv("OPENAI_API_BASE");
    if (base && base[0]) util_str_copy(cfg->base_url, sizeof(cfg->base_url), base);

    const char *key = getenv("XZERO_API_KEY");
    if (!key) key = getenv("OPENAI_API_KEY");
    if (key) util_str_copy(cfg->api_key, sizeof(cfg->api_key), key);

    const char *model = getenv("XZERO_MODEL");
    if (!model) model = getenv("OPENAI_MODEL");
    if (model && model[0]) util_str_copy(cfg->model, sizeof(cfg->model), model);
}

static bool load_file(const char *path, XZeroConfig *cfg) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <=0 || sz > 1024*1024) { fclose(f); return false; }
    char *buf = (char*)malloc(sz+1);
    if (!buf) { fclose(f); return false; }
    size_t r = fread(buf, 1, sz, f);
    fclose(f);
    buf[r]='\0';
    cJSON *json = cJSON_Parse(buf);
    free(buf);
    if (!json) return false;
    cJSON *bu = cJSON_GetObjectItem(json, "base_url");
    cJSON *ak = cJSON_GetObjectItem(json, "api_key");
    cJSON *mo = cJSON_GetObjectItem(json, "model");
    // also support baseUrl / apiKey camelCase
    if (!bu) bu = cJSON_GetObjectItem(json, "baseUrl");
    if (!ak) ak = cJSON_GetObjectItem(json, "apiKey");
    if (cJSON_IsString(bu) && bu->valuestring) util_str_copy(cfg->base_url, sizeof(cfg->base_url), bu->valuestring);
    if (cJSON_IsString(ak) && ak->valuestring) util_str_copy(cfg->api_key, sizeof(cfg->api_key), ak->valuestring);
    if (cJSON_IsString(mo) && mo->valuestring) util_str_copy(cfg->model, sizeof(cfg->model), mo->valuestring);
    cJSON_Delete(json);
    return true;
}

bool config_load(XZeroConfig *cfg, const char *override_path) {
    if (!cfg) return false;
    config_init_defaults(cfg);
    bool found=false;
    if (override_path && override_path[0]) {
        if (load_file(override_path, cfg)) found=true;
    } else {
        // try global then local (local overrides global)
        char global[XZERO_PATH_MAX];
        char local[XZERO_PATH_MAX];
        config_get_global_path(global, sizeof(global));
        config_get_local_path(local, sizeof(local));
        if (config_exists(global)) {
            if (load_file(global, cfg)) found=true;
        }
        // local may be ./xzero.json or ./.xzero/config.json - check both
        if (config_exists("./xzero.json")) {
            load_file("./xzero.json", cfg);
            found=true;
        } else if (config_exists("./.xzero/config.json")) {
            load_file("./.xzero/config.json", cfg);
            found=true;
        } else if (config_exists(local) && strcmp(local, "./xzero.json")!=0) {
            load_file(local, cfg);
        }
    }
    // env overrides last
    config_apply_env_overrides(cfg);
    // normalize url
    char norm[XZERO_URL_MAX];
    util_url_normalize(cfg->base_url, norm, sizeof(norm));
    util_str_copy(cfg->base_url, sizeof(cfg->base_url), norm);
    return found;
}

bool config_save(const XZeroConfig *cfg, const char *override_path) {
    if (!cfg) return false;
    char path[XZERO_PATH_MAX];
    if (override_path && override_path[0]) util_str_copy(path, sizeof(path), override_path);
    else config_get_global_path(path, sizeof(path));

    // ensure dir exists
    char dir[XZERO_PATH_MAX];
    util_str_copy(dir, sizeof(dir), path);
    char *sep = strrchr(dir, '/');
    char *sep2 = strrchr(dir, '\\');
    if (sep2 && (!sep || sep2>sep)) sep=sep2;
    if (sep) {
        *sep='\0';
        util_mkdir_p(dir);
    }

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "base_url", cfg->base_url);
    cJSON_AddStringToObject(json, "api_key", cfg->api_key);
    cJSON_AddStringToObject(json, "model", cfg->model);
    char *out = cJSON_Print(json);
    cJSON_Delete(json);
    if (!out) return false;
    FILE *f = fopen(path, "wb");
    if (!f) { free(out); return false; }
    fwrite(out, 1, strlen(out), f);
    fclose(f);
    free(out);
#ifndef _WIN32
    chmod(path, 0600);
#endif
    return true;
}
