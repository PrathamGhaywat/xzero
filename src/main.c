#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "config.h"
#include "prompt.h"
#include "openai.h"
#include "http.h"
#include "session.h"
#include "agent.h"
#include "repl.h"

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
static void init_console(void){
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    // Enable ANSI escape sequences
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if(hOut != INVALID_HANDLE_VALUE){
        DWORD mode=0;
        if(GetConsoleMode(hOut, &mode)) SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if(hIn != INVALID_HANDLE_VALUE){
        DWORD mode=0;
        if(GetConsoleMode(hIn, &mode)) SetConsoleMode(hIn, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    // Ensure stdout is not translated
    // Do not use _O_U8TEXT (breaks printf), just keep binary
}
#else
static void init_console(void){}
#endif

static void print_usage(const char *prog){
    printf("xzero v0.1 - C coding agent (cross-platform)\n");
    printf("Usage: %s [options] [prompt]\n", prog);
    printf("\nOptions:\n");
    printf("  --base-url URL      OpenAI compatible base URL\n");
    printf("  --api-key KEY       API key (blank for none)\n");
    printf("  --model NAME        Model name (default gpt-4o-mini)\n");
    printf("  --config PATH       Config file path\n");
    printf("  --resume ID         Resume session ID\n");
    printf("  --list-sessions     List sessions\n");
    printf("  --test              Test connection and exit\n");
    printf("  --help              Show help\n");
    printf("  --version           Show version\n");
    printf("\nConfig: XZERO_BASE_URL, XZERO_API_KEY, XZERO_MODEL env vars override file.\n");
    printf("Files: %%APPDATA%%/xzero/config.json (Windows), ~/.config/xzero/config.json (Linux), etc.\n");
}

static void print_version(void){
    printf("xzero 0.1.0 (C17, libcurl, cJSON)\n");
}

int main(int argc, char **argv){
    init_console();
    http_global_init();
    srand((unsigned int)time(NULL));

    XZeroConfig cfg;
    const char *override_path=NULL;
    const char *cli_base=NULL, *cli_key=NULL, *cli_model=NULL;
    const char *resume_id=NULL;
    bool do_test=false, do_list=false;
    const char *prompt_arg=NULL;

    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"--help")==0||strcmp(argv[i],"-h")==0){ print_usage(argv[0]); http_global_cleanup(); return 0; }
        else if(strcmp(argv[i],"--version")==0){ print_version(); http_global_cleanup(); return 0; }
        else if(strcmp(argv[i],"--base-url")==0 && i+1<argc) cli_base=argv[++i];
        else if(strcmp(argv[i],"--api-key")==0 && i+1<argc) cli_key=argv[++i];
        else if(strcmp(argv[i],"--model")==0 && i+1<argc) cli_model=argv[++i];
        else if(strcmp(argv[i],"--config")==0 && i+1<argc) override_path=argv[++i];
        else if(strcmp(argv[i],"--resume")==0 && i+1<argc) resume_id=argv[++i];
        else if(strcmp(argv[i],"--test")==0) do_test=true;
        else if(strcmp(argv[i],"--list-sessions")==0) do_list=true;
        else if(argv[i][0]!='-'){
            // first non-flag is prompt (join rest)
            prompt_arg=argv[i];
            // join remaining args as prompt if multiple words
            if(i+1<argc){
                static char joined[8192]="";
                strncpy(joined, prompt_arg, sizeof(joined)-1);
                for(int j=i+1;j<argc;j++){
                    strncat(joined," ",sizeof(joined)-strlen(joined)-1);
                    strncat(joined,argv[j],sizeof(joined)-strlen(joined)-1);
                }
                prompt_arg=joined;
            }
            break;
        } else {
            fprintf(stderr,"Unknown option %s\n", argv[i]);
            print_usage(argv[0]);
            http_global_cleanup();
            return 1;
        }
    }

    if(do_list){
        char ids[64][64];
        int cnt=0;
        session_list(ids,&cnt,64);
        printf("Sessions (%d):\n", cnt);
        for(int i=0;i<cnt;i++) printf("  %s\n", ids[i]);
        http_global_cleanup();
        return 0;
    }

    config_load(&cfg, override_path);
    if(cli_base) strncpy(cfg.base_url, cli_base, sizeof(cfg.base_url)-1);
    if(cli_key) strncpy(cfg.api_key, cli_key, sizeof(cfg.api_key)-1);
    if(cli_model) strncpy(cfg.model, cli_model, sizeof(cfg.model)-1);

    // Interactive prompt if missing (unless --test wants to show error)
    bool need_prompt = false;
    if(!cfg.base_url[0]) need_prompt=true;
    // base_url has default, so only prompt if user explicitly deleted?

    char cfg_path[XZERO_PATH_MAX];
    if(override_path) strncpy(cfg_path, override_path, sizeof(cfg_path)-1);
    else config_get_global_path(cfg_path,sizeof(cfg_path));
    bool cfg_exists = config_exists(override_path ? override_path : cfg_path) || config_exists("./xzero.json") || config_exists("./.xzero/config.json");

    // If --test is requested, never prompt interactively - just test current cfg (including CLI overrides)
    if(do_test){
        char err[256];
        if(!config_validate(&cfg, err, sizeof(err))){
            fprintf(stderr,"Config invalid: %s\n", err);
            http_global_cleanup();
            return 1;
        }
        char test_err[512];
        printf("Testing %s ...\n", cfg.base_url);
        if(openai_test_connection(&cfg, test_err, sizeof(test_err))==0){
            printf("\xE2\x9C\x93 Connected (%s) model=%s api_key=%s\n", cfg.base_url, cfg.model, cfg.api_key[0]?"set":"(blank)");
            // If config didn't exist before and CLI was provided, save it now
            if(!cfg_exists && (cli_base||cli_key||cli_model)){
                config_save(&cfg, override_path);
                printf("Saved config to %s\n", cfg_path);
            }
            http_global_cleanup();
            return 0;
        } else {
            fprintf(stderr,"Failed: %s\n", test_err);
            http_global_cleanup();
            return 1;
        }
    }

    // First-run interactive setup only if no config exists and no CLI overrides provided
    if(!cfg_exists && !cli_base && !cli_key && !cli_model){
        printf("xzero first run — configure OpenAI compatible endpoint\n");
        char buf[XZERO_URL_MAX];
        if(prompt_input("Enter OpenAI base URL", cfg.base_url, buf, sizeof(buf))){
            strncpy(cfg.base_url, buf, sizeof(cfg.base_url)-1);
        }
        char keybuf[XZERO_KEY_MAX];
        if(prompt_password("Enter API Key (leave blank if none): ", keybuf, sizeof(keybuf))){
            strncpy(cfg.api_key, keybuf, sizeof(cfg.api_key)-1);
            memset(keybuf,0,sizeof(keybuf));
        }
        char modelbuf[XZERO_MODEL_MAX];
        if(prompt_input("Enter model", cfg.model, modelbuf, sizeof(modelbuf))){
            strncpy(cfg.model, modelbuf, sizeof(cfg.model)-1);
        }
        char err[256];
        if(!config_validate(&cfg, err, sizeof(err))){
            fprintf(stderr,"Config invalid: %s\n", err);
            http_global_cleanup();
            return 1;
        }
        printf("Testing connection to %s ...\n", cfg.base_url);
        char test_err[512];
        if(openai_test_connection(&cfg, test_err, sizeof(test_err))!=0){
            fprintf(stderr,"Connection test failed: %s\n", test_err);
            if(!prompt_confirm("Save config anyway?", false)){
                http_global_cleanup();
                return 1;
            }
        } else {
            printf("\xE2\x9C\x93 Connected\n");
        }
        if(!config_save(&cfg, override_path)){
            fprintf(stderr,"Warning: failed to save config to %s\n", cfg_path);
        } else {
            printf("Saved config to %s\n", cfg_path);
        }
    } else if(!cfg_exists && (cli_base||cli_key||cli_model)){
        // CLI provided on first run - validate and save silently (no prompt)
        char err[256];
        if(!config_validate(&cfg, err, sizeof(err))){
            fprintf(stderr,"Config invalid: %s\n", err);
            http_global_cleanup();
            return 1;
        }
        // Save for next runs
        config_save(&cfg, override_path);
    }

    // Special: config show?
    // Handle "config --show" as prompt_arg
    if(prompt_arg && (strcmp(prompt_arg,"config --show")==0 || strcmp(prompt_arg,"config")==0)){
        char masked[128];
        config_mask_key(cfg.api_key, masked, sizeof(masked));
        printf("base_url: %s\nmodel: %s\napi_key: %s\nconfig: %s\n", cfg.base_url, cfg.model, masked, cfg_path);
        http_global_cleanup();
        return 0;
    }

    // Create/load session
    Session *sess=NULL;
    if(resume_id){
        sess=(Session*)calloc(1,sizeof(Session));
        if(!session_load(sess, resume_id)){
            fprintf(stderr,"Failed to load session %s, creating new\n", resume_id);
            free(sess);
            sess=session_create(cfg.model);
        }
        strncpy(sess->model, cfg.model, sizeof(sess->model)-1);
    } else {
        sess=session_create(cfg.model);
    }

    int rc=0;
    if(prompt_arg && prompt_arg[0] && strcmp(prompt_arg,"config --show")!=0){
        AgentOpts opts={&cfg, sess, 32, false};
        rc=agent_run_turn(&opts, prompt_arg);
        session_save(sess);
    } else {
        rc=repl_run(&cfg, sess);
    }
    session_free(sess);
    http_global_cleanup();
    return rc;
}
