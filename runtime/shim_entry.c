/* v2_capi_shim, built against the target's (main's) headers: loads one extension built against the v2.0.1 contract and
 * hands it the v2.0.1 function table, implemented on top of the target's. The child is named by V2_CAPI_SHIM_LOAD
 * (a path); its entrypoint is <file name up to the first dot>_init_c_api_v2. */
/* one loader per contract, named after it: built with -DDUCKDB_EXTENSION_NAME=v2_capi_shim_v201
 * -DSHIM_CONTRACT_VERSION='"v2.0.1"' (the defaults) */
#ifndef DUCKDB_EXTENSION_NAME
#define DUCKDB_EXTENSION_NAME v2_capi_shim_v201
#endif
#ifndef SHIM_CONTRACT_VERSION
#define SHIM_CONTRACT_VERSION "v2.0.1"
#endif
#include "duckdb_extension_v2.h"
DUCKDB_EXTENSION_EXTERN
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the v2.0.1-layout table (compat_table.c, built against the contract's headers) */
extern const char compat_table[];

/* set when the child fetched its table: DuckDB refuses an extension that never does, and so does the loader */
static int child_fetched_api;
/* whether the child already reported a failure in the error slot */
static int child_failed(duckdb_v2_error_info_handle err) {
	DUCKDB_V2_ERROR code = DUCKDB_V2_ERROR_NONE;
	return err && duckdb_v2_error_info_get_code(err, &code) == DUCKDB_V2_ERROR_NONE && code != DUCKDB_V2_ERROR_NONE;
}

/* get_api as the contract declares it: (extension, version), no length */
static const void *contract_get_api(duckdb_v2_extension_handle extension, const char *version) {
	(void)extension;
	/* like DuckDB's own get_api for a version-pinned extension: the name is the build's identity (a tag or a commit),
	 * not a request; the contract was chosen by the file's footer */
	if (getenv("V2_CAPI_SHIM_TRACE")) fprintf(stderr, "v2_capi_shim: get_api('%s')\n", version ? version : "(null)");
	child_fetched_api = 1;
	return (const void *)compat_table;
}

static void report(duckdb_v2_error_info_handle *err, const char *what, const char *detail) {
	char text[1024];
	snprintf(text, sizeof(text), DUCKDB_EXTENSION_STR(DUCKDB_EXTENSION_NAME) ": %s: %s", what, detail ? detail : "");
	duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_INPUT_INVALID);
#ifdef SHIM_TARGET_STR_STRUCT /* a target that still takes duckdb_v2_str (v2.0.x) */
	duckdb_v2_str t = {text, strlen(text)};
	duckdb_v2_error_info_set_text(*err, &t);
#else
	duckdb_v2_error_info_set_text(*err, text, strlen(text));
#endif
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
	child_fetched_api = 0;
	init(&input);
	if (!child_fetched_api && !child_failed(*err)) {
		report(err, path, "the extension did not fetch its API table (DUCKDB_EXTENSION_API_INIT): refusing it");
	}
}
