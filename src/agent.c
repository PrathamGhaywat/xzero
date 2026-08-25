#include "agent.h"
#include "openai.h"
#include "http.h"
#include "stream.h"
#include "tools/registry.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>

static char *make_url(const char *base, const char *path){
    size_t bl=strlen(base);
    bool slash = bl>0 && base[bl-1]=='/';
    bool pslash = path[0]=='/';
    char *out=(char*)malloc(bl+strlen(path)+2);
    if(!out) return NULL;
    if(slash && pslash) snprintf(out, bl+strlen(path)+2, "%s%s", base, path+1);
    else if(!slash && !pslash) snprintf(out, bl+strlen(path)+2, "%s/%s", base, path);
    else snprintf(out, bl+strlen(path)+2, "%s%s", base, path);
    return out;
}

int agent_run_turn(AgentOpts *opts, const char *user_message){
    if(!opts || !opts->cfg || !opts->session) return -1;
    if(user_message && user_message[0]){
        cJSON *msg=cJSON_CreateObject();
        cJSON_AddStringToObject(msg,"role","user");
        cJSON_AddStringToObject(msg,"content",user_message);
        session_append(opts->session, msg);
        cJSON_Delete(msg);
        session_save(opts->session);
    }

    int iter=0;
    int max_iter = opts->max_iterations ? opts->max_iterations : 32;

    while(iter < max_iter){
        iter++;
        // Build payload with cache-aware ordering
        cJSON *messages = session_messages_for_api(opts->session);
        cJSON *tools = tools_build_openai_array();
        char *payload = openai_build_payload(opts->cfg, messages, tools, true);
        cJSON_Delete(messages);
        cJSON_Delete(tools);
        if(!payload){ fprintf(stderr,"[xzero] oom building payload\n"); return -1; }

        char auth_buf[1200];
        const char *headers[8]={0};
        openai_build_headers(opts->cfg, headers, 8, auth_buf, sizeof(auth_buf));

        char *url = make_url(opts->cfg->base_url, "/chat/completions");
        if(!url){ free(payload); return -1; }

        if(opts->verbose) fprintf(stderr,"[xzero] iter %d -> %s\n", iter, url);

        StreamResult result;
        stream_result_init(&result);
        // Lever #2: instrument cache miss - if provider returns cached_tokens vs prompt_tokens
        int rc = stream_chat(url, payload, headers, &result);
        free(payload);
        free(url);
        if(rc!=0){
            fprintf(stderr,"\n[xzero] HTTP stream error (iter %d). Check base_url/model/api_key.\n", iter);
            if(result.content_delta && result.content_len>0) printf("\n%s\n", result.content_delta);
            if(result.finish_reason) fprintf(stderr,"finish_reason: %s\n", result.finish_reason);
            stream_result_free(&result);
            // Don't hard-fail: append error to session so LLM can see, but also allow user to retry
            cJSON *err_msg=cJSON_CreateObject();
            cJSON_AddStringToObject(err_msg,"role","system");
            cJSON_AddStringToObject(err_msg,"content","[xzero] HTTP error - previous LLM call failed. Try again or check connection.]");
            session_append(opts->session, err_msg);
            cJSON_Delete(err_msg);
            return -1;
        }
        // Ensure newline after streaming content
        if(result.content_len>0) printf("\n");

        // Build assistant message for history
        cJSON *assistant = cJSON_CreateObject();
        cJSON_AddStringToObject(assistant,"role","assistant");
        if(result.content_delta && result.content_len>0){
            cJSON_AddStringToObject(assistant,"content", result.content_delta);
        } else {
            cJSON_AddStringToObject(assistant,"content", "");
        }
        int tool_n = result.tool_calls ? cJSON_GetArraySize(result.tool_calls) : 0;
        if(tool_n>0){
            cJSON *tc_arr=cJSON_CreateArray();
            for(int i=0;i<tool_n;i++){
                cJSON *tc = cJSON_GetArrayItem(result.tool_calls, i);
                cJSON *name = cJSON_GetObjectItem(tc,"name");
                cJSON *id = cJSON_GetObjectItem(tc,"id");
                cJSON *args = cJSON_GetObjectItem(tc,"arguments");
                if(!name || !cJSON_IsString(name) || !name->valuestring[0]) continue;
                cJSON *out=cJSON_CreateObject();
                cJSON_AddStringToObject(out,"id", cJSON_IsString(id)? id->valuestring : "call_0");
                cJSON_AddStringToObject(out,"type","function");
                cJSON *func=cJSON_CreateObject();
                cJSON_AddStringToObject(func,"name", name->valuestring);
                cJSON_AddStringToObject(func,"arguments", cJSON_IsString(args)? args->valuestring : "{}");
                cJSON_AddItemToObject(out,"function",func);
                cJSON_AddItemToArray(tc_arr, out);
            }
            if(cJSON_GetArraySize(tc_arr)>0) cJSON_AddItemToObject(assistant,"tool_calls",tc_arr);
            else cJSON_Delete(tc_arr);
        }
        session_append(opts->session, assistant);
        cJSON_Delete(assistant);

        // If no tool calls, we are done (Lever #4: deterministic stop - when every tool in batch finishes, loop stops)
        if(tool_n==0 || cJSON_GetArraySize(result.tool_calls)==0 || (result.tool_calls && cJSON_GetArraySize(result.tool_calls)==0)){
            // No tools -> final answer
            if(result.finish_reason) {
                if(opts->verbose) fprintf(stderr,"[xzero] finish: %s (prompt %d, completion %d)\n", result.finish_reason, result.prompt_tokens, result.completion_tokens);
                // Instrument cache miss: if prompt_tokens high vs cached, warn
                if(result.prompt_tokens>0 && result.completion_tokens>=0){
                    // Heuristic: if prompt_tokens > 5000 and no caching, every turn resends full context at full price
                    // We already structured for cache hits via stable prefix + session cache key
                }
            }
            stream_result_free(&result);
            session_save(opts->session);
            break;
        }

        // Execute tool calls in batch (Lever #4: fan of deterministic tools, no sub-agents)
        // Print batch header
        printf("\n[xzero] %d tool call(s)\n", tool_n);
        for(int i=0;i<tool_n;i++){
            cJSON *tc=cJSON_GetArrayItem(result.tool_calls,i);
            cJSON *name=cJSON_GetObjectItem(tc,"name");
            cJSON *id=cJSON_GetObjectItem(tc,"id");
            cJSON *args=cJSON_GetObjectItem(tc,"arguments");
            const char *tname = (name && cJSON_IsString(name))? name->valuestring : "unknown";
            const char *tid = (id && cJSON_IsString(id))? id->valuestring : "";
            const char *targs = (args && cJSON_IsString(args))? args->valuestring : "{}";
            // Pretty-print exact command/args for human
            {
                cJSON *j = cJSON_Parse(targs);
                if(j){
                    if(strcmp(tname,"bash")==0){
                        cJSON *c = cJSON_GetObjectItem(j,"command");
                        const char *cmd = (c && cJSON_IsString(c)) ? c->valuestring : targs;
                        printf(" > bash [%s] `$ %s`\n", tid, cmd);
                    } else if(strcmp(tname,"read")==0){
                        cJSON *p = cJSON_GetObjectItem(j,"path");
                        cJSON *off = cJSON_GetObjectItem(j,"offset");
                        cJSON *lim = cJSON_GetObjectItem(j,"limit");
                        const char *path = (p && cJSON_IsString(p)) ? p->valuestring : "?";
                        if(cJSON_IsNumber(off) || cJSON_IsNumber(lim)){
                            int o = cJSON_IsNumber(off)? off->valueint : 0;
                            int l = cJSON_IsNumber(lim)? lim->valueint : 0;
                            printf(" > read [%s] `%s` (offset %d limit %d)\n", tid, path, o, l);
                        } else {
                            printf(" > read [%s] `%s`\n", tid, path);
                        }
                    } else if(strcmp(tname,"write")==0 || strcmp(tname,"edit")==0){
                        cJSON *p = cJSON_GetObjectItem(j,"path");
                        const char *path = (p && cJSON_IsString(p)) ? p->valuestring : "?";
                        printf(" > %s [%s] `%s`\n", tname, tid, path);
                    } else if(strcmp(tname,"grep")==0){
                        cJSON *pat = cJSON_GetObjectItem(j,"pattern");
                        cJSON *pa = cJSON_GetObjectItem(j,"path");
                        const char *pat_s = (pat && cJSON_IsString(pat)) ? pat->valuestring : "?";
                        const char *pa_s = (pa && cJSON_IsString(pa)) ? pa->valuestring : ".";
                        printf(" > grep [%s] pattern=`%s` path=`%s`\n", tid, pat_s, pa_s);
                    } else if(strcmp(tname,"glob")==0){
                        cJSON *pat = cJSON_GetObjectItem(j,"pattern");
                        cJSON *pa = cJSON_GetObjectItem(j,"path");
                        const char *pat_s = (pat && cJSON_IsString(pat)) ? pat->valuestring : "?";
                        const char *pa_s = (pa && cJSON_IsString(pa)) ? pa->valuestring : ".";
                        printf(" > glob [%s] pattern=`%s` path=`%s`\n", tid, pat_s, pa_s);
                    } else {
                        printf(" > %s [%s] %s\n", tname, tid, targs);
                    }
                    cJSON_Delete(j);
                } else {
                    printf(" > %s %s\n", tname, tid);
                    if(opts->verbose) printf("   args: %.200s\n", targs);
                }
            }

            // Approval for write/edit/bash
            if(tools_needs_approval(tname)){
                printf("   Approve %s? [y/N]: ", tname);
                fflush(stdout);
                char buf[16];
                if(fgets(buf,sizeof(buf),stdin)){
                    if(buf[0]!='y' && buf[0]!='Y'){
                        cJSON *tool_msg=cJSON_CreateObject();
                        cJSON_AddStringToObject(tool_msg,"role","tool");
                        cJSON_AddStringToObject(tool_msg,"tool_call_id", tid);
                        cJSON_AddStringToObject(tool_msg,"content","permission denied by user");
                        session_append(opts->session, tool_msg);
                        cJSON_Delete(tool_msg);
                        continue;
                    }
                }
            }

            ToolResult tr = tools_dispatch(tname, targs, opts->session);
            // Add result as tool message
            cJSON *tool_msg=cJSON_CreateObject();
            cJSON_AddStringToObject(tool_msg,"role","tool");
            // OpenAI expects tool_call_id for tool role
            cJSON_AddStringToObject(tool_msg,"tool_call_id", tid);
            // Some servers expect "content" as string
            cJSON_AddStringToObject(tool_msg,"content", tr.content ? tr.content : "");
            // Alternative for older API: also add name?
            session_append(opts->session, tool_msg);
            cJSON_Delete(tool_msg);

            // Human already saw details via tool's printf; model gets capped/truncated content
            if(tr.truncated && tr.full_path){
                if(opts->verbose) fprintf(stderr,"[xzero] %s output truncated, full at %s\n", tname, tr.full_path);
            }
            if(tr.is_error && opts->verbose) fprintf(stderr,"[xzero] tool %s error\n", tname);
            tool_result_free(&tr);
        }
        stream_result_free(&result);
        session_save(opts->session);
        // Loop continues to next LLM call with tool results (no "let me summarize" turn - Lever #4)
    }
    if(iter>=max_iter){
        printf("\n[xzero] max iterations (%d) reached\n", max_iter);
    }
    return 0;
}
