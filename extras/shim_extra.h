/* duckdb_shim_extra/1: what the loader offers its children beyond the v2 contract, fetched with
 * get_api(extension, "duckdb_shim_extra/1"). Plain C over opaque handles, so it means the same to a child built against
 * any contract; each loader implements it on its own DuckDB version however it can. Add functions only at the end. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define DUCKDB_SHIM_EXTRA_V1 "duckdb_shim_extra/1"

/* one result row: column values as text, NULL pointer for SQL NULL */
typedef void (*duckdb_shim_row_fn)(void *user_data, uint64_t column_count, const char *const *values,
                                   const uint64_t *lengths);

typedef struct {
	/* G1: runs SQL on a connection of its own to the database of a context (a callback's or an entrypoint's). Rows of
	 * the last statement go to row (may be NULL). Returns false and the error text in error (NUL-terminated, cut to
	 * error_capacity) on failure */
	bool (*context_query)(struct _duckdb_v2_context *context, const char *sql, uint64_t sql_length,
	                      duckdb_shim_row_fn row, void *user_data, char *error, uint64_t error_capacity);
	/* G2: whether the query the context runs has been interrupted (Ctrl-C, max execution time) */
	bool (*context_interrupted)(struct _duckdb_v2_context *context);
} duckdb_shim_extra_v1;
