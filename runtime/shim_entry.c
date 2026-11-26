/* v2_capi_shim, built against the target's (main's) headers: loads one extension built against the v2.0.1 contract and
 * hands it the v2.0.1 function table, implemented on top of the target's. The child is named by V2_CAPI_SHIM_LOAD
 * (a path); its entrypoint is <file name up to the first dot>_init_c_api_v2. */
#define DUCKDB_EXTENSION_NAME v2_capi_shim
#include "duckdb_extension_v2.h"
DUCKDB_EXTENSION_EXTERN
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the v2.0.1-layout table (compat_table.c, built against the contract's headers) */
extern const char compat_table[];

/* get_api as the contract declares it: (extension, version), no length */
static const void *contract_get_api(duckdb_v2_extension_handle extension, const char *version) {
	(void)extension;
	return (version && strncmp(version, "v2.", 3) == 0) ? (const void *)compat_table : NULL;
}

static void report(duckdb_v2_error_info_handle *err, const char *what, const char *detail) {
	char text[1024];
	snprintf(text, sizeof(text), "v2_capi_shim: %s: %s", what, detail ? detail : "");
	duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_error_info_set_text(*err, text, strlen(text));
}

DUCKDB_EXTENSION_ENTRYPOINT(duckdb_v2_extension_handle extension, duckdb_v2_context_handle context,
                            duckdb_v2_error_info_handle *err) {
	const char *path = getenv("V2_CAPI_SHIM_LOAD");
	if (!path || !*path) {
		return; /* nothing to load */
	}
	void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!lib) {
		report(err, "dlopen failed", dlerror());
		return;
	}
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	char symbol[256];
	size_t len = strcspn(base, ".");
	snprintf(symbol, sizeof(symbol), "%.*s_init_c_api_v2", (int)len, base);
	void (*init)(struct duckdb_v2_extension_input *) = (void (*)(struct duckdb_v2_extension_input *))dlsym(lib, symbol);
	if (!init) {
		report(err, "entrypoint not found", symbol);
		return;
	}
	/* the contract's input has the same layout; only get_api's signature differs */
	struct duckdb_v2_extension_input input;
	input.get_api = (duckdb_v2_extension_get_api_fn)(void (*)(void))contract_get_api;
	input.extension = extension;
	input.context = context;
	input.err = err;
	init(&input);
}
