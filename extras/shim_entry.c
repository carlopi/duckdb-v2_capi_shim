/* v2_capi_shim, built against the target's (main's) headers. Loads extensions built against the v2.0.1 contract and
 * hands them the v2.0.1 function table implemented on the target's, plus, on request, duckdb_shim_extra/1.
 *   at LOAD:  the child named by V2_CAPI_SHIM_LOAD (a path), if set
 *   later:    SELECT * FROM load_extension('path')
 * A child's entrypoint is <file name up to the first dot>_init_c_api_v2. */
#define DUCKDB_EXTENSION_NAME v2_capi_shim
#include "duckdb_extension_v2.h"
DUCKDB_EXTENSION_EXTERN
#include "shim_extra.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char compat_table[];        /* the v2.0.1-layout table (compat_table.c) */
const void *shim_extra_table(void);      /* shim_extra.cpp */
bool shim_invoke_child(duckdb_v2_context_handle ctx, const char *name, void *lib, char *error, uint64_t cap);

/* get_api as the contract declares it: (extension, version), no length */
static const void *contract_get_api(duckdb_v2_extension_handle extension, const char *version) {
	(void)extension;
	if (!version) return NULL;
	if (strncmp(version, "v2.", 3) == 0) return compat_table;
	return NULL;
}

static void report(duckdb_v2_error_info_handle *err, const char *what, const char *detail) {
	char text[1024];
	snprintf(text, sizeof(text), "v2_capi_shim: %s: %s", what, detail ? detail : "");
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
		void (*offer)(const void *, const char *) = (void (*)(const void *, const char *))dlsym(pending_lib, pending_offer);
		if (offer) offer(shim_extra_table(), DUCKDB_SHIM_EXTRA_V1);
	}
	struct duckdb_v2_extension_input contract = *input; /* same layout; only get_api's signature differs */
	contract.get_api = (duckdb_v2_extension_get_api_fn)(void (*)(void))contract_get_api;
	pending_child(&contract);
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
	child_init_fn init = NULL;
	void *lib = open_child(buf, name, sizeof(name), &init, error, sizeof(error));
	if (!lib) { report(err, buf, error); return; }
	pending_child = init;
	pending_lib = lib;
	snprintf(pending_offer, sizeof(pending_offer), "%s_duckdb_shim_extra", name);
	if (!shim_invoke_child(context, name, lib, error, sizeof(error))) { report(err, buf, error); return; }
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
	duckdb_v2_table_function_set_bind_callback(fn, load_bind, NULL);
	duckdb_v2_table_function_set_exec_callback(fn, load_exec, NULL);
	duckdb_v2_extension_register_table_function(extension, fn, NULL);
	duckdb_v2_table_function_destroy(&fn);
	duckdb_v2_logical_type_destroy(&varchar);
}

DUCKDB_EXTENSION_ENTRYPOINT(duckdb_v2_extension_handle extension, duckdb_v2_context_handle context,
                            duckdb_v2_error_info_handle *err) {
	register_load_extension(extension, context);
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
