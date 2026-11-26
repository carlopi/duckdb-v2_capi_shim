/* v2_capi_shim_info() and v2_capi_shim_capabilities(): what this loader does, for users and for tests to check.
 * Fixed rows of text, registered through the target's v2 API (either flavour: a target that still takes duckdb_v2_str
 * is built with -DSHIM_TARGET_STR_STRUCT). The target is baked in at build time: -DSHIM_TARGET_VERSION,
 * -DSHIM_TARGET_SOURCE_ID, -DSHIM_PLATFORM; the capabilities by the includer, as SHIM_CAPABILITIES. */
#pragma once
#include <string.h>

#ifndef SHIM_TARGET_VERSION
#define SHIM_TARGET_VERSION "unknown"
#endif
#ifndef SHIM_TARGET_SOURCE_ID
#define SHIM_TARGET_SOURCE_ID "unknown"
#endif
#ifndef SHIM_PLATFORM
#define SHIM_PLATFORM "unknown"
#endif

#ifdef SHIM_TARGET_STR_STRUCT
#define SHIM_TEXT(s) (&(duckdb_v2_str) {(s), strlen(s)})
#define SHIM_NO_TYPE_PARAMS NULL, NULL, 0
#else
#define SHIM_TEXT(s) (s), strlen(s)
#define SHIM_NO_TYPE_PARAMS NULL, NULL, NULL, 0
#endif

typedef struct {
	const char *name;
	int columns;
	const char *const *column_names;
	int rows;
	const char *const *cells; /* rows x columns */
} shim_table;

typedef struct { const shim_table *table; int emitted; } shim_table_state;
static void shim_table_state_free(void *p) { free(p); }

static void shim_table_bind(duckdb_v2_function_bind_info_handle info, duckdb_v2_table_function_bind_info_handle result,
                            duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	(void)err;
	void *user = NULL;
	duckdb_v2_function_bind_get_user_data(info, &user, NULL);
	const shim_table *t = (const shim_table *)user;
	duckdb_v2_factory_handle factory = NULL;
	duckdb_v2_logical_type_handle varchar = NULL;
	duckdb_v2_context_get_factory(context, &factory, NULL);
	duckdb_v2_factory_create_type_from_id(factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR, SHIM_NO_TYPE_PARAMS, &varchar, NULL);
	for (int c = 0; c < t->columns; c++) {
		duckdb_v2_table_function_bind_add_result_column(result, SHIM_TEXT(t->column_names[c]), varchar, NULL);
	}
	duckdb_v2_logical_type_destroy(&varchar);
	shim_table_state *state = (shim_table_state *)calloc(1, sizeof(shim_table_state));
	state->table = t;
	duckdb_v2_opaque data = {state, shim_table_state_free, NULL};
	duckdb_v2_function_bind_set_bind_data(info, &data, NULL);
}

static void shim_table_exec(duckdb_v2_table_function_exec_info_handle info, duckdb_v2_context_handle context,
                            duckdb_v2_error_info_handle *err) {
	(void)err;
	void *p = NULL;
	duckdb_v2_table_function_exec_get_bind_data(info, &p, NULL);
	shim_table_state *state = (shim_table_state *)p;
	const shim_table *t = state->table;
	duckdb_v2_data_chunk_handle chunk = NULL;
	duckdb_v2_factory_handle factory = NULL;
	duckdb_v2_table_function_exec_get_output_chunk(info, &chunk, NULL);
	duckdb_v2_context_get_factory(context, &factory, NULL);
	int n = state->emitted ? 0 : t->rows;
	state->emitted = 1;
	for (int c = 0; c < t->columns; c++) {
		duckdb_v2_vector_handle vector = NULL;
		duckdb_v2_data_chunk_get_vector(chunk, (idx_t)c, &vector, NULL);
		duckdb_v2_vector_set_size(vector, (idx_t)n, NULL);
		for (int r = 0; r < n; r++) {
			duckdb_v2_value_handle value = NULL;
			duckdb_v2_value_create_varchar(factory, SHIM_TEXT(t->cells[r * t->columns + c]), &value, NULL);
			duckdb_v2_vector_set_value(vector, (idx_t)r, value, NULL);
			duckdb_v2_value_destroy(&value);
		}
	}
}

static void shim_register_table(duckdb_v2_extension_handle extension, duckdb_v2_context_handle context,
                                const shim_table *t) {
	duckdb_v2_factory_handle factory = NULL;
	duckdb_v2_table_function_handle fn = NULL;
	duckdb_v2_context_get_factory(context, &factory, NULL);
	duckdb_v2_table_function_create(factory, &fn, NULL);
	duckdb_v2_table_function_set_name(fn, SHIM_TEXT(t->name), NULL);
	duckdb_v2_opaque user = {(void *)t, NULL, NULL};
	duckdb_v2_table_function_set_user_data(fn, &user, NULL);
	duckdb_v2_table_function_set_bind_callback(fn, shim_table_bind, NULL);
	duckdb_v2_table_function_set_exec_callback(fn, shim_table_exec, NULL);
	duckdb_v2_extension_register_table_function(extension, fn, NULL);
	duckdb_v2_table_function_destroy(&fn);
}

static const char *const SHIM_INFO_COLUMNS[] = {"contract", "target_version", "target_source_id", "platform", "kind"};
static const char *const SHIM_INFO_CELLS[] = {SHIM_CONTRACT_VERSION, SHIM_TARGET_VERSION, SHIM_TARGET_SOURCE_ID,
                                              SHIM_PLATFORM, SHIM_KIND};
static const shim_table SHIM_INFO = {"v2_capi_shim_info", 5, SHIM_INFO_COLUMNS, 1, SHIM_INFO_CELLS};
/* the named function tables shipped beyond the contract: what a child can receive through <name>_duckdb_shim_extra */
static const char *const SHIM_CAPABILITY_COLUMNS[] = {"name", "version", "functions", "description"};
static const shim_table SHIM_CAPABILITIES_TABLE = {"v2_capi_shim_capabilities", 4, SHIM_CAPABILITY_COLUMNS,
                                                   (int)(sizeof(SHIM_CAPABILITIES) / sizeof(SHIM_CAPABILITIES[0])) / 4 -
                                                       SHIM_CAPABILITIES_EMPTY,
                                                   SHIM_CAPABILITIES};

static void shim_register_info(duckdb_v2_extension_handle extension, duckdb_v2_context_handle context) {
	shim_register_table(extension, context, &SHIM_INFO);
	shim_register_table(extension, context, &SHIM_CAPABILITIES_TABLE);
}
