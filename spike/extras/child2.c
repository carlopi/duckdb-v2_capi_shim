/* A plain v2 extension built against the v2.0.1 contract, which also asks for duckdb_shim_extra/1 and works without it.
 *   child2_sql(q)     runs q through context_query from inside the exec callback, one text row per result row
 *   child2_wait(ms)   waits up to ms milliseconds, stopping early when context_interrupted says so
 * Without the extras (loaded natively) both fail with a message saying so. */
#define DUCKDB_EXTENSION_NAME child2
#include "duckdb_extension_v2.h"
DUCKDB_EXTENSION_EXTERN
#include "shim_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* capabilities beyond v2.0.1: connections is required (the load is refused without it), interrupts optional */
static const duckdb_shim_connections_v1 *connections;
static const duckdb_shim_interrupts_v1 *interrupts;

static duckdb_v2_str S(const char *s) {
	duckdb_v2_str r = {s, strlen(s)};
	return r;
}
static void fail(duckdb_v2_error_info_handle *err, const char *text) {
	duckdb_v2_str t = S(text);
	duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_error_info_set_text(*err, &t);
}
static duckdb_v2_logical_type_handle varchar_type(duckdb_v2_context_handle ctx) {
	duckdb_v2_factory_handle f = NULL;
	duckdb_v2_logical_type_handle t = NULL;
	duckdb_v2_context_get_factory(ctx, &f, NULL);
	duckdb_v2_factory_create_type_from_id(f, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR, NULL, NULL, 0, &t, NULL);
	return t;
}

/* bind data: the argument as text, and the rows once computed */
typedef struct { char arg[4096]; char **rows; size_t count, next; int done; } state_t;
static void free_state(void *p) {
	state_t *s = (state_t *)p;
	for (size_t i = 0; i < s->count; i++) free(s->rows[i]);
	free(s->rows);
	free(s);
}
static void add_row(state_t *s, const char *text) {
	s->rows = (char **)realloc(s->rows, (s->count + 1) * sizeof(char *));
	s->rows[s->count++] = strdup(text);
}
static void bind(duckdb_v2_function_bind_info_handle info, duckdb_v2_table_function_bind_info_handle result,
                 duckdb_v2_context_handle ctx, duckdb_v2_error_info_handle *err) {
	duckdb_v2_value_handle v = NULL;
	duckdb_v2_str text = {0, 0};
	duckdb_v2_function_bind_get_arg_value(info, 0, &v, NULL);
	duckdb_v2_value_get_varchar(v, &text, NULL);
	state_t *s = (state_t *)calloc(1, sizeof(state_t));
	snprintf(s->arg, sizeof(s->arg), "%.*s", (int)text.len, text.ptr ? text.ptr : "");
	duckdb_v2_value_destroy(&v);
	duckdb_v2_logical_type_handle t = varchar_type(ctx);
	duckdb_v2_str col = S("result");
	duckdb_v2_table_function_bind_add_result_column(result, &col, t, NULL);
	duckdb_v2_logical_type_destroy(&t);
	duckdb_v2_opaque data = {s, free_state, NULL};
	duckdb_v2_function_bind_set_bind_data(info, &data, NULL);
}
static void emit(duckdb_v2_table_function_exec_info_handle info, duckdb_v2_context_handle ctx, state_t *s) {
	duckdb_v2_data_chunk_handle chunk = NULL;
	duckdb_v2_vector_handle vec = NULL;
	duckdb_v2_factory_handle f = NULL;
	duckdb_v2_table_function_exec_get_output_chunk(info, &chunk, NULL);
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, NULL);
	duckdb_v2_context_get_factory(ctx, &f, NULL);
	size_t n = s->count - s->next > 1024 ? 1024 : s->count - s->next;
	duckdb_v2_vector_set_size(vec, n, NULL);
	for (size_t i = 0; i < n; i++) {
		duckdb_v2_value_handle v = NULL;
		duckdb_v2_str t = S(s->rows[s->next + i]);
		duckdb_v2_value_create_varchar(f, &t, &v, NULL);
		duckdb_v2_vector_set_value(vec, i, v, NULL);
		duckdb_v2_value_destroy(&v);
	}
	s->next += n;
}
static state_t *state_of(duckdb_v2_table_function_exec_info_handle info) {
	void *p = NULL;
	duckdb_v2_table_function_exec_get_bind_data(info, &p, NULL);
	return (state_t *)p;
}

static void on_row(void *ud, uint64_t ncols, const char *const *vals, const uint64_t *lens) {
	char line[4096];
	size_t at = 0;
	for (uint64_t c = 0; c < ncols && at < sizeof(line) - 1; c++) {
		at += (size_t)snprintf(line + at, sizeof(line) - at, "%s%.*s", c ? " | " : "", vals[c] ? (int)lens[c] : 4,
		                       vals[c] ? vals[c] : "NULL");
	}
	add_row((state_t *)ud, line);
}
static void sql_exec(duckdb_v2_table_function_exec_info_handle info, duckdb_v2_context_handle ctx,
                     duckdb_v2_error_info_handle *err) {
	state_t *s = state_of(info);
	if (!s->done) {
		s->done = 1;
		char error[1024];
		if (!connections->context_query(ctx, s->arg, strlen(s->arg), on_row, s, error, sizeof(error))) {
			fail(err, error);
			return;
		}
	}
	emit(info, ctx, s);
}
static void wait_exec(duckdb_v2_table_function_exec_info_handle info, duckdb_v2_context_handle ctx,
                      duckdb_v2_error_info_handle *err) {
	(void)err;
	state_t *s = state_of(info);
	if (!s->done) {
		s->done = 1;
		long total = atol(s->arg), waited = 0;
		struct timespec step = {0, 20 * 1000 * 1000};
		while (waited < total && !(interrupts && interrupts->context_interrupted(ctx))) {
			nanosleep(&step, NULL);
			waited += 20;
		}
		char line[128];
		snprintf(line, sizeof(line), waited < total ? "interrupted after %ld ms" : "waited %ld ms", waited);
		add_row(s, line);
	}
	emit(info, ctx, s);
}

static void reg(duckdb_v2_extension_handle ext, duckdb_v2_context_handle ctx, const char *name, const char *param,
                duckdb_v2_table_function_exec_callback_fn exec) {
	duckdb_v2_factory_handle f = NULL;
	duckdb_v2_table_function_handle fn = NULL;
	duckdb_v2_function_signature_handle sig = NULL;
	duckdb_v2_context_get_factory(ctx, &f, NULL);
	duckdb_v2_logical_type_handle t = varchar_type(ctx);
	duckdb_v2_str n = S(name), p = S(param);
	duckdb_v2_table_function_create(f, &fn, NULL);
	duckdb_v2_table_function_set_name(fn, &n, NULL);
	duckdb_v2_table_function_get_signature(fn, &sig, NULL);
	duckdb_v2_function_signature_add_parameter(sig, &p, t, NULL, DUCKDB_V2_FUNCTION_PARAMETER_KIND_STANDARD, NULL);
	duckdb_v2_table_function_set_bind_callback(fn, bind, NULL);
	duckdb_v2_table_function_set_exec_callback(fn, exec, NULL);
	duckdb_v2_extension_register_table_function(ext, fn, NULL);
	duckdb_v2_table_function_destroy(&fn);
	duckdb_v2_logical_type_destroy(&t);
}

/* Outside the v2 contract: a loader that has capabilities offers them here before the entrypoint; DuckDB never calls
 * this. (get_api cannot carry them: for a version-pinned extension DuckDB's get_api ignores the name and returns the
 * whole v2 table; for a stable one, an unknown name fails the load) */
DUCKDB_CAPI_ENTRY_VISIBILITY void child2_duckdb_shim_extra(const char *capability, const void *table) {
	if (strcmp(capability, DUCKDB_SHIM_CONNECTIONS_V1) == 0) connections = (const duckdb_shim_connections_v1 *)table;
	if (strcmp(capability, DUCKDB_SHIM_INTERRUPTS_V1) == 0) interrupts = (const duckdb_shim_interrupts_v1 *)table;
}

/* v2.0.1 + capi('connections'): works where that is offered, refused at LOAD elsewhere */
DUCKDB_EXTENSION_ENTRYPOINT(duckdb_v2_extension_handle extension, duckdb_v2_context_handle context,
                            duckdb_v2_error_info_handle *err) {
	if (!connections) {
		fail(err, "child2 requires v2.0.1 + capability 'connections/1', which this DuckDB does not offer (load it "
		          "through v2_capi_shim)");
		return;
	}
	reg(extension, context, "child2_sql", "query", sql_exec);
	reg(extension, context, "child2_wait", "ms", wait_exec);
}
