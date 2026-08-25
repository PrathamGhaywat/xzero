#include "../src/util.h"
#include "../src/config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

int main(void){
    char out[XZERO_URL_MAX];
    util_url_normalize(" https://api.openai.com/v1/ ", out, sizeof(out));
    assert(strcmp(out,"https://api.openai.com/v1")==0);
    util_url_normalize("http://localhost:11434/v1/", out, sizeof(out));
    assert(strcmp(out,"http://localhost:11434/v1")==0);
    XZeroConfig cfg;
    config_init_defaults(&cfg);
    assert(strcmp(cfg.base_url,"https://api.openai.com/v1")==0);
    char err[256];
    assert(config_validate(&cfg,err,sizeof(err)));
    cfg.base_url[0]='\0';
    assert(!config_validate(&cfg,err,sizeof(err)));
    const char *big="a\nb\nc\n";
    bool trunc=false;
    size_t lines=0;
    char *capped=util_cap_output(big, strlen(big), "/tmp/foo", &trunc, &lines);
    assert(!trunc);
    free(capped);
    char *large=(char*)malloc(60*1024);
    memset(large,'x',60*1024-1);
    large[60*1024-1]='\0';
    char *capped2=util_cap_output(large, 60*1024-1, "/tmp/spill.txt", &trunc, &lines);
    assert(trunc);
    assert(strstr(capped2,"Full output")!=NULL);
    free(capped2);
    free(large);
    printf("test_config passed\n");
    return 0;
}
