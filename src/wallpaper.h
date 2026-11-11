#pragma once
#include <stddef.h>
#include <stdbool.h>

// Generate the built-in "Sigma Wolf: Cringe Edition" BMP.
// Cross-platform (useful for tests); returns true on success.
bool wallpaper_generate_default(char *out_path, size_t out_n, char *err, size_t err_n);

// Set the Windows desktop wallpaper. `path` may be NULL/empty to use the
// built-in generated image, a local image path, or an http(s) image URL.
// Returns 0 on success, -1 on error (including on non-Windows platforms,
// where the image is still prepared and its path is reported).
int wallpaper_set(const char *path, char *err, size_t err_n);
