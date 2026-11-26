/* The running DuckDB's own v2 function table, fetched when the first child loads (ShimForwardChild): the adapters call
 * through it, as any v2 extension does, so the code that runs is the running DuckDB's, not this extension's copy */
#include "duckdb_extension_v2.h"
duckdb_ext_api_v2 duckdb_ext_api = {0};

void shim_set_target_api(const void *api) {
	if (api) {
		duckdb_ext_api = *(const duckdb_ext_api_v2 *)api;
	}
}
