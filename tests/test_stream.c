#include "../src/stream.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(void){
    StreamResult r;
    stream_result_init(&r);
    StreamState s;
    stream_state_init(&s, &r, false);
    const char *chunk1="data: {\"choices\":[{\"delta\":{\"content\":\"Hello \"}}]}\n\n";
    const char *chunk2="data: {\"choices\":[{\"delta\":{\"content\":\"world\"}}]}\n\ndata: [DONE]\n\n";
    stream_sse_callback(chunk1, strlen(chunk1), &s);
    stream_sse_callback(chunk2, strlen(chunk2), &s);
    assert(r.content_delta && strcmp(r.content_delta,"Hello world")==0);
    assert(r.done);
    stream_state_free(&s);
    stream_result_free(&r);
    printf("test_stream passed\n");
    return 0;
}
