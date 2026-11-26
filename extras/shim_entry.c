/* v2_capi_shim, built against the target's (main's) headers. Loads extensions built against the v2.0.1 contract and
 * hands them the v2.0.1 function table implemented on the target's, plus, on request, duckdb_shim_extra/1.
 *   at LOAD:  the child named by V2_CAPI_SHIM_LOAD (a path), if set
 *   later:    SELECT * FROM load_extension('path')
 * A child's entrypoint is <file name up to the first dot>_init_c_api_v2. */
/* the contract is baked in per build (-DSHIM_CONTRACT_VERSION, default v2.0.1; it shows in the footer version and
 * the description); the extension is always v2_capi_shim. Built with -DDUCKDB_EXTENSION_NAME=v2_capi_shim
 * -DSHIM_CONTRACT_VERSION='"v2.0.1"' (the defaults) */
#ifndef DUCKDB_EXTENSION_NAME
#define DUCKDB_EXTENSION_NAME v2_capi_shim
#endif
#ifndef SHIM_CONTRACT_VERSION
#define SHIM_CONTRACT_VERSION "v2.0.1"
#endif
#include "duckdb_extension_v2.h"
DUCKDB_EXTENSION_EXTERN
#include <stdlib.h>
#define SHIM_KIND "bridge+capabilities"
static const char *const SHIM_CAPABILITIES[] = {"connections/1", "context_query: SQL on a connection of its own to the database of a callback's context", "interrupts/1", "context_interrupted: whether the query of a context was interrupted (Ctrl-C, max_execution_time)"};
#define SHIM_CAPABILITIES_EMPTY 0
#include "shim_tables.h"
#include "shim_extra.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char compat_table[];        /* the v2.0.1-layout table (compat_table.c) */
void shim_offer_all(duckdb_shim_offer_fn offer); /* shim_extra.cpp */
bool shim_verify_child(duckdb_v2_context_handle ctx, const char *path, const char *repo, const char *contract, char *error, uint64_t cap);
void shim_describe_self(duckdb_v2_context_handle ctx, const char *name, const char *contract);
bool shim_invoke_child(duckdb_v2_context_handle ctx, const char *name, const char *path, void *lib, char *error, uint64_t cap);

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
	return compat_table;
}

static void report(duckdb_v2_error_info_handle *err, const char *what, const char *detail) {
	char text[1024];
	snprintf(text, sizeof(text), DUCKDB_EXTENSION_STR(DUCKDB_EXTENSION_NAME) ": %s: %s", what, detail ? detail : "");
	duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_error_info_set_text(*err, text, strlen(text));
}

typedef void (*child_init_fn)(struct duckdb_v2_extension_input *);

/* the child about to be initialized, and the trampoline DuckDB's invoker calls instead of it */
static child_init_fn pending_child;
static char pending_offer[300];
static void *pending_lib;
void shim_forward_child(struct duckdb_v2_extension_input *input) {
	/* DuckDB checks that the extension fetched its table: fetch it on the child's behalf (the bridge forwards to it) */
	if (input->get_api && !input->get_api(input->extension, DUCKDB_EXTENSION_API_VERSION_STRING, strlen(DUCKDB_EXTENSION_API_VERSION_STRING))) {
		return; /* DuckDB recorded why */
	}
	/* the extras, to a child that asks for them by exporting <name>_duckdb_shim_extra */
	if (pending_lib) {
		duckdb_shim_offer_fn offer = (duckdb_shim_offer_fn)dlsym(pending_lib, pending_offer);
		if (offer) shim_offer_all(offer);
	}
	struct duckdb_v2_extension_input contract = *input; /* same layout; only get_api's signature differs */
	contract.get_api = (duckdb_v2_extension_get_api_fn)(void (*)(void))contract_get_api;
	child_fetched_api = 0;
	pending_child(&contract);
	if (!child_fetched_api && !child_failed(*input->err)) {
		report(input->err, "child", "the extension did not fetch its API table (DUCKDB_EXTENSION_API_INIT): refusing it");
	}
}

/* dlopen + entrypoint lookup; returns the library, its name in name (the file base) */
static void *open_child(const char *path, char *name, size_t name_cap, child_init_fn *init, char *error, size_t cap) {
	void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!lib) {
		snprintf(error, cap, "dlopen failed: %s", dlerror());
		return NULL;
	}
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	snprintf(name, name_cap, "%.*s", (int)strcspn(base, "."), base);
	char symbol[300];
	snprintf(symbol, sizeof(symbol), "%s_init_c_api_v2", name);
	*init = (child_init_fn)dlsym(lib, symbol);
	if (!*init) {
		snprintf(error, cap, "entrypoint %s not found", symbol);
		return NULL;
	}
	return lib;
}

/* load_extension(path): bind does the work, exec reports it as one row */
typedef struct { char name[256]; int emitted; } load_state;
static void free_state(void *p) { free(p); }

static void load_bind(duckdb_v2_function_bind_info_handle info, duckdb_v2_table_function_bind_info_handle result,
                      duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	duckdb_v2_value_handle arg = NULL;
	const char *path = NULL;
	idx_t path_len = 0;
	char error[1024] = "", name[256] = "", buf[4096];
	if (duckdb_v2_function_bind_get_arg_value(info, 0, &arg, NULL) || duckdb_v2_value_get_varchar(arg, &path, &path_len, NULL)) {
		report(err, "load_extension", "expects a path");
		return;
	}
	snprintf(buf, sizeof(buf), "%.*s", (int)path_len, path);
	duckdb_v2_value_destroy(&arg);
	char repo[256] = "";
	idx_t repo_index = 0;
	bool repo_found = false;
	if (duckdb_v2_function_bind_get_arg_index(info, "repo", 4, &repo_index, &repo_found, NULL) == DUCKDB_V2_ERROR_NONE && repo_found) {
		duckdb_v2_value_handle rv = NULL;
		const char *r = NULL;
		idx_t rl = 0;
		if (duckdb_v2_function_bind_get_arg_value(info, repo_index, &rv, NULL) == DUCKDB_V2_ERROR_NONE &&
		    duckdb_v2_value_get_varchar(rv, &r, &rl, NULL) == DUCKDB_V2_ERROR_NONE && r) {
			snprintf(repo, sizeof(repo), "%.*s", (int)rl, r);
		}
		duckdb_v2_value_destroy(&rv);
	}
	if (!shim_verify_child(context, buf, repo, SHIM_CONTRACT_VERSION, error, sizeof(error))) {
		char what[4200];
		snprintf(what, sizeof(what), "refusing %s", buf);
		report(err, what, error);
		return;
	}
	child_init_fn init = NULL;
	void *lib = open_child(buf, name, sizeof(name), &init, error, sizeof(error));
	if (!lib) { report(err, buf, error); return; }
	pending_child = init;
	pending_lib = lib;
	snprintf(pending_offer, sizeof(pending_offer), "%s_duckdb_shim_extra", name);
	if (!shim_invoke_child(context, name, buf, lib, error, sizeof(error))) { report(err, buf, error); return; }
	duckdb_v2_factory_handle factory = NULL;
	duckdb_v2_logical_type_handle varchar = NULL;
	duckdb_v2_context_get_factory(context, &factory, NULL);
	duckdb_v2_factory_create_type_from_id(factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR, NULL, NULL, NULL, 0, &varchar, NULL);
	duckdb_v2_table_function_bind_add_result_column(result, "loaded", 6, varchar, NULL);
	duckdb_v2_logical_type_destroy(&varchar);
	load_state *state = (load_state *)calloc(1, sizeof(load_state));
	snprintf(state->name, sizeof(state->name), "%s", name);
	duckdb_v2_opaque data = {state, free_state, NULL};
	duckdb_v2_function_bind_set_bind_data(info, &data, NULL);
}

static void load_exec(duckdb_v2_table_function_exec_info_handle info, duckdb_v2_context_handle context,
                      duckdb_v2_error_info_handle *err) {
	(void)err;
	void *p = NULL;
	duckdb_v2_table_function_exec_get_bind_data(info, &p, NULL);
	load_state *state = (load_state *)p;
	duckdb_v2_data_chunk_handle chunk = NULL;
	duckdb_v2_vector_handle vector = NULL;
	duckdb_v2_table_function_exec_get_output_chunk(info, &chunk, NULL);
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vector, NULL);
	if (state->emitted) { duckdb_v2_vector_set_size(vector, 0, NULL); return; }
	state->emitted = 1;
	duckdb_v2_factory_handle factory = NULL;
	duckdb_v2_value_handle value = NULL;
	duckdb_v2_context_get_factory(context, &factory, NULL);
	duckdb_v2_value_create_varchar(factory, state->name, strlen(state->name), &value, NULL);
	duckdb_v2_vector_set_size(vector, 1, NULL);
	duckdb_v2_vector_set_value(vector, 0, value, NULL);
	duckdb_v2_value_destroy(&value);
}

static void register_load_extension(duckdb_v2_extension_handle extension, duckdb_v2_context_handle context) {
	duckdb_v2_factory_handle factory = NULL;
	duckdb_v2_table_function_handle fn = NULL;
	duckdb_v2_function_signature_handle sig = NULL;
	duckdb_v2_logical_type_handle varchar = NULL;
	duckdb_v2_context_get_factory(context, &factory, NULL);
	duckdb_v2_factory_create_type_from_id(factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR, NULL, NULL, NULL, 0, &varchar, NULL);
	duckdb_v2_table_function_create(factory, &fn, NULL);
	duckdb_v2_table_function_set_name(fn, "load_extension", 14, NULL);
	duckdb_v2_table_function_get_signature(fn, &sig, NULL);
	duckdb_v2_function_signature_add_parameter(sig, "path", 4, varchar, NULL, DUCKDB_V2_FUNCTION_PARAMETER_KIND_STANDARD, NULL);
	/* the repository whose keys signed the file; NULL default, required unless allow_unsigned_extensions */
	duckdb_v2_value_handle null_default = NULL;
	duckdb_v2_value_create_null(factory, varchar, &null_default, NULL);
	duckdb_v2_function_signature_add_parameter(sig, "repo", 4, varchar, null_default, DUCKDB_V2_FUNCTION_PARAMETER_KIND_NAMED_ONLY, NULL);
	duckdb_v2_value_destroy(&null_default);
	duckdb_v2_table_function_set_bind_callback(fn, load_bind, NULL);
	duckdb_v2_table_function_set_exec_callback(fn, load_exec, NULL);
	duckdb_v2_extension_register_table_function(extension, fn, NULL);
	duckdb_v2_table_function_destroy(&fn);
	duckdb_v2_logical_type_destroy(&varchar);
}

DUCKDB_EXTENSION_ENTRYPOINT(duckdb_v2_extension_handle extension, duckdb_v2_context_handle context,
                            duckdb_v2_error_info_handle *err) {
	shim_register_info(extension, context);
	register_load_extension(extension, context);
	shim_describe_self(context, DUCKDB_EXTENSION_STR(DUCKDB_EXTENSION_NAME), SHIM_CONTRACT_VERSION);
	const char *path = getenv("V2_CAPI_SHIM_LOAD");
	if (!path || !*path) return;
	char error[1024], name[256];
	child_init_fn init = NULL;
	void *lib_at_load = open_child(path, name, sizeof(name), &init, error, sizeof(error));
	if (!lib_at_load) { report(err, path, error); return; }
	pending_child = init;
	pending_lib = lib_at_load;
	snprintf(pending_offer, sizeof(pending_offer), "%s_duckdb_shim_extra", name);
	struct duckdb_v2_extension_input input = {(duckdb_v2_extension_get_api_fn)(void (*)(void))NULL, extension, context, err};
	shim_forward_child(&input);
}
