#pragma once
#include "config.h"
#include "session.h"
#include <stdbool.h>

// Bare while-loop agent (Lever #4: no planner/critic/sub-agents)
// Deterministic batch tool execution, stops when no tool_calls.
typedef struct {
    XZeroConfig *cfg;
    Session *session;
    int max_iterations; // default 32
    bool verbose;
} AgentOpts;

int agent_run_turn(AgentOpts *opts, const char *user_message);
int agent_run_loop(AgentOpts *opts); // REPL integrated
