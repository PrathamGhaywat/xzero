#include "repl.h"
#include "agent.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void print_help(void){
    printf("Commands:\n");
    printf("  /help          Show this help\n");
    printf("  /clear         Clear screen\n");
    printf("  /exit, /quit   Exit\n");
    printf("  /model <name>  Change model\n");
    printf("  /sessions      List sessions\n");
    printf("  /new           New session (abandoned branch deleted)\n");
    printf("  /compact       Force compaction\n");
    printf("\n");
}

int repl_run(XZeroConfig *cfg, Session *session){
    if(!cfg || !session) return -1;
    printf("xzero v0.1 — model %s @ %s\n", cfg->model, cfg->base_url);
    printf("Session %s  |  type /help for commands, Ctrl+C to interrupt\n\n", session->id);

    char line[8192];
    AgentOpts opts={cfg, session, 32, false};

    while(1){
        printf("\033[1;34mxzero>\033[0m ");
        fflush(stdout);
        if(!fgets(line,sizeof(line),stdin)){
            printf("\n");
            break;
        }
        size_t len=strlen(line);
        while(len>0 && (line[len-1]=='\n' || line[len-1]=='\r')) line[--len]='\0';
        // trim
        char *p=line;
        while(*p==' '||*p=='\t') p++;
        if(p[0]=='\0') continue;

        if(p[0]=='/'){
            if(strcmp(p,"/help")==0){ print_help(); continue; }
            if(strcmp(p,"/exit")==0||strcmp(p,"/quit")==0) break;
            if(strcmp(p,"/clear")==0){ printf("\033[2J\033[H"); continue; }
            if(strncmp(p,"/model ",7)==0){
                const char *m=p+7;
                while(*m==' ') m++;
                if(*m){ strncpy(cfg->model, m, sizeof(cfg->model)-1); printf("Model -> %s\n", cfg->model); }
                continue;
            }
            if(strcmp(p,"/sessions")==0){
                char ids[32][64];
                int cnt=0;
                session_list(ids,&cnt,32);
                printf("Sessions (%d):\n", cnt);
                for(int i=0;i<cnt;i++) printf("  %s%s\n", ids[i], strcmp(ids[i], session->id)==0?" (current)":"");
                continue;
            }
            if(strcmp(p,"/new")==0){
                // Lever #3: abandoned branches deleted
                // For now just create new session and delete old if empty? Actually user wants new -> old branch kept but not summarized if abandoned
                // We treat /new as compact + new session, old session saved but not loaded
                Session *ns = session_create(cfg->model);
                if(ns){
                    session_save(session);
                    printf("New session %s (old %s saved, abandoned detour will be deleted if not resumed)\n", ns->id, session->id);
                    // Replace session in place
                    // Copy ns to session (shallow)
                    // Instead, free old messages and copy
                    if(session->messages) cJSON_Delete(session->messages);
                    session->messages=ns->messages; ns->messages=NULL;
                    strncpy(session->id, ns->id, sizeof(session->id)-1);
                    strncpy(session->file_path, ns->file_path, sizeof(session->file_path)-1);
                    session->estimated_tokens=ns->estimated_tokens;
                    for(int i=0;i<session->touched_count;i++) free(session->touched_files[i]);
                    session->touched_count=ns->touched_count;
                    for(int i=0;i<ns->touched_count;i++) session->touched_files[i]=ns->touched_files[i];
                    ns->touched_count=0;
                    session->summary[0]='\0';
                    free(ns);
                }
                continue;
            }
            if(strcmp(p,"/compact")==0){
                session_maybe_compact(session);
                printf("Compacted. Tokens ~%zu, messages %d\n", session->estimated_tokens, session->messages? cJSON_GetArraySize(session->messages):0);
                continue;
            }
            printf("Unknown command %s (try /help)\n", p);
            continue;
        }

        // Regular user message -> agent turn
        agent_run_turn(&opts, p);
        printf("\n");
    }
    session_save(session);
    return 0;
}
