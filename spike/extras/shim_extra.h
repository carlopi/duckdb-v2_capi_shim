/* duckdb_shim_extra/1: what the loader offers its children beyond the v2 contract, fetched with
 * get_api(extension, "duckdb_shim_extra/1"). Plain C over opaque handles, so it means the same to a child built against
 * any contract; each loader implements it on its own DuckDB version however it can. Add functions only at the end. */
#pragma once
#include <stdbool.h>
#include <stdint.h>


/* one result row: column values as text, NULL pointer for SQL NULL */
typedef void (*duckdb_shim_row_fn)(void *user_data, uint64_t column_count, const char *const *values,
                                   const uint64_t *lengths);

/* capabilities, offered one by one by name; a table only ever grows at the end */
#define DUCKDB_SHIM_CONNECTIONS_V1 "connections/1"
typedef struct {
	/* G1: runs SQL on a connection of its own to the database of a context (a callback's or an entrypoint's). Rows of
	 * the last statement go to row (may be NULL). Returns false and the error text in error (NUL-terminated, cut to
	 * error_capacity) on failure */
	bool (*context_query)(struct _duckdb_v2_context *context, const char *sql, uint64_t sql_length,
	                      duckdb_shim_row_fn row, void *user_data, char *error, uint64_t error_capacity);
} duckdb_shim_connections_v1;

#define DUCKDB_SHIM_INTERRUPTS_V1 "interrupts/1"
typedef struct {
	/* G2: whether the query the context runs has been interrupted (Ctrl-C, max_execution_time) */
	bool (*context_interrupted)(struct _duckdb_v2_context *context);
} duckdb_shim_interrupts_v1;

/* What a child exports to receive capabilities: called by a loader, before the entrypoint, once per capability it
 * offers; never by DuckDB itself. A child that requires one refuses the load in its entrypoint when it was not offered */
typedef void (*duckdb_shim_offer_fn)(const char *capability, const void *table);
