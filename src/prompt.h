#pragma once
#include <stddef.h>
#include <stdbool.h>

bool prompt_input(const char *prompt, const char *def, char *out, size_t n);
bool prompt_password(const char *prompt, char *out, size_t n);
bool prompt_confirm(const char *prompt, bool def_yes);
