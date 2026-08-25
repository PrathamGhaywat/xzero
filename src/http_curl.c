#include "http.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <curl/curl.h>

struct mem {
    char *data;
    size_t len;
    size_t cap;
};

static size_t write_mem(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size*nmemb;
    struct mem *m = (struct mem*)userp;
    if (m->len + realsize + 1 > m->cap) {
        size_t newcap = m->cap ? m->cap*2 : 8192;
        while (newcap < m->len + realsize + 1) newcap*=2;
        char *ptr = (char*)realloc(m->data, newcap);
        if (!ptr) return 0;
        m->data = ptr;
        m->cap = newcap;
    }
    memcpy(m->data + m->len, contents, realsize);
    m->len += realsize;
    m->data[m->len]='\0';
    return realsize;
}

struct stream_ctx {
    http_stream_cb cb;
    void *user;
};

static size_t write_stream(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size*nmemb;
    struct stream_ctx *ctx = (struct stream_ctx*)userp;
    if (ctx->cb) return ctx->cb((const char*)contents, realsize, ctx->user) ? realsize : 0;
    return realsize;
}

void http_response_free(HttpResponse *r) {
    if (!r) return;
    free(r->body);
    free(r->headers);
    r->body=NULL;
    r->headers=NULL;
    r->body_len=0;
}

void http_global_init(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}
void http_global_cleanup(void) {
    curl_global_cleanup();
}

static struct curl_slist* build_headers(const char * const *headers) {
    struct curl_slist *list=NULL;
    if (!headers) return NULL;
    for (int i=0; headers[i]; i++) {
        list = curl_slist_append(list, headers[i]);
    }
    return list;
}

static CURL* setup_common(const char *url, const char * const *headers, struct curl_slist **out_list) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 2048L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    struct curl_slist *list = build_headers(headers);
    if (list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
        if (out_list) *out_list=list;
        else {
            // caller must free but we keep reference via curl?
            // curl doesn't copy list, so we need to keep it; but for non-stream we manage elsewhere
        }
    }
    return curl;
}

int http_get(const char *url, const char * const *headers, HttpResponse *out) {
    if (!url || !out) return -1;
    memset(out, 0, sizeof(*out));
    struct curl_slist *list=NULL;
    CURL *curl = setup_common(url, headers, &list);
    if (!curl) { if(list) curl_slist_free_all(list); return -1; }
    struct mem chunk={0};
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_mem);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);
    CURLcode res = curl_easy_perform(curl);
    long code=0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    out->status = code;
    out->body = chunk.data;
    out->body_len = chunk.len;
    if (res != CURLE_OK) {
        if (out->body==NULL) {
            const char *err = curl_easy_strerror(res);
            out->body = strdup(err);
            out->body_len = out->body ? strlen(out->body) : 0;
        }
        curl_slist_free_all(list);
        curl_easy_cleanup(curl);
        return -1;
    }
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
    return 0;
}

int http_post_json(const char *url, const char *json_body, const char * const *headers, HttpResponse *out) {
    if (!url || !out) return -1;
    memset(out, 0, sizeof(*out));
    struct curl_slist *list=NULL;
    CURL *curl = setup_common(url, headers, &list);
    if (!curl) { if(list) curl_slist_free_all(list); return -1; }
    struct mem chunk={0};
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    if (json_body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(json_body));
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_mem);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);
    CURLcode res = curl_easy_perform(curl);
    long code=0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    out->status=code;
    out->body=chunk.data;
    out->body_len=chunk.len;
    if (res != CURLE_OK) {
        if (out->body==NULL) {
            const char *err=curl_easy_strerror(res);
            out->body=strdup(err);
            out->body_len=out->body?strlen(out->body):0;
        }
        curl_slist_free_all(list);
        curl_easy_cleanup(curl);
        return -1;
    }
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
    return 0;
}

int http_post_stream(const char *url, const char *json_body, const char * const *headers, http_stream_cb cb, void *user) {
    if (!url || !cb) return -1;
    struct curl_slist *list=NULL;
    CURL *curl = setup_common(url, headers, &list);
    if (!curl) { if(list) curl_slist_free_all(list); return -1; }
    struct stream_ctx ctx={cb, user};
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    if (json_body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(json_body));
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_stream);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    // Disable buffering
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) return -1;
    return 0;
}
