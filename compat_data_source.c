/* Hand-written, for a target whose table functions and COPY FROM scan with data sources (DuckDB #29411): in the
 * contract, scan callbacks are set on the table function (or the copy function's COPY FROM side); in the target they
 * belong to a data source, which the function copies when it is attached. So each function created through the
 * contract gets a record: the factory it was created with and, once a scan callback is set, a data source, which every
 * scan callback setter updates and re-attaches (the function keeps a copy as of each attach). The getters of the scan's
 * info handles are generated (renamed onto the data source); the callbacks receive data source infos, which the
 * contract's callbacks take as their own (opaque pointers alike). */
#include <pthread.h>
#include <stdlib.h>
#include "duckdb_v2.h"

typedef struct ds_rec {
	void *function; /* duckdb_v2_table_function_handle or duckdb_v2_copy_function_handle */
	duckdb_v2_factory_handle factory;
	duckdb_v2_data_source_handle source;
	struct ds_rec *next;
} ds_rec;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static ds_rec *recs;

static void remember(void *function, duckdb_v2_factory_handle factory) {
	ds_rec *r = (ds_rec *)calloc(1, sizeof(ds_rec));
	r->function = function;
	r->factory = factory;
	pthread_mutex_lock(&lock);
	r->next = recs;
	recs = r;
	pthread_mutex_unlock(&lock);
}

static void forget(void *function) {
	ds_rec *found = NULL;
	pthread_mutex_lock(&lock);
	for (ds_rec **it = &recs; *it; it = &(*it)->next) {
		if ((*it)->function == function) {
			found = *it;
			*it = found->next;
			break;
		}
	}
	pthread_mutex_unlock(&lock);
	if (found) {
		duckdb_v2_data_source_destroy(&found->source);
		free(found);
	}
}

/* the data source of a function, created on first use; NULL (and the error set) when the function is not one created
 * through the contract or the data source cannot be made */
static duckdb_v2_data_source_handle source_of(void *function, duckdb_v2_error_info_handle *err) {
	ds_rec *r;
	pthread_mutex_lock(&lock);
	for (r = recs; r && r->function != function; r = r->next) {
	}
	pthread_mutex_unlock(&lock);
	if (!r) {
		if (err && *err) {
			duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_INPUT_INVALID);
		}
		return NULL;
	}
	if (!r->source && duckdb_v2_data_source_create(r->factory, &r->source, err) != DUCKDB_V2_ERROR_NONE) {
		return NULL;
	}
	return r->source;
}

DUCKDB_V2_ERROR compat_duckdb_v2_table_function_create(duckdb_v2_factory_handle factory,
                                                       duckdb_v2_table_function_handle *function,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_V2_ERROR rc = duckdb_v2_table_function_create(factory, function, err);
	if (rc == DUCKDB_V2_ERROR_NONE && function && *function) {
		remember(*function, factory);
	}
	return rc;
}

DUCKDB_V2_ERROR compat_duckdb_v2_table_function_destroy(duckdb_v2_table_function_handle *function) {
	if (function && *function) {
		forget(*function);
	}
	return duckdb_v2_table_function_destroy(function);
}

DUCKDB_V2_ERROR compat_duckdb_v2_copy_function_create(duckdb_v2_factory_handle factory,
                                                      duckdb_v2_copy_function_handle *function,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_V2_ERROR rc = duckdb_v2_copy_function_create(factory, function, err);
	if (rc == DUCKDB_V2_ERROR_NONE && function && *function) {
		remember(*function, factory);
	}
	return rc;
}

DUCKDB_V2_ERROR compat_duckdb_v2_copy_function_destroy(duckdb_v2_copy_function_handle *function) {
	if (function && *function) {
		forget(*function);
	}
	return duckdb_v2_copy_function_destroy(function);
}

/* a scan callback setter: set on the function's data source, which is then attached again */
#define TABLE_SETTER(what, cb_type)                                                                                     \
	DUCKDB_V2_ERROR compat_duckdb_v2_table_function_set_##what##_callback(duckdb_v2_table_function_handle function,     \
	                                                                     void (*callback)(void),                         \
	                                                                     duckdb_v2_error_info_handle *err) {             \
		duckdb_v2_data_source_handle source = source_of(function, err);                                                 \
		if (!source) {                                                                                                  \
			return DUCKDB_V2_ERROR_INPUT_INVALID;                                                                       \
		}                                                                                                               \
		DUCKDB_V2_ERROR rc = duckdb_v2_data_source_set_##what##_callback(source, (cb_type)callback, err);               \
		return rc != DUCKDB_V2_ERROR_NONE ? rc : duckdb_v2_table_function_set_data_source(function, source, err);       \
	}
#define COPY_SETTER(what, cb_type)                                                                                      \
	DUCKDB_V2_ERROR compat_duckdb_v2_copy_from_set_##what##_callback(duckdb_v2_copy_function_handle function,           \
	                                                                void (*callback)(void),                              \
	                                                                duckdb_v2_error_info_handle *err) {                  \
		duckdb_v2_data_source_handle source = source_of(function, err);                                                 \
		if (!source) {                                                                                                  \
			return DUCKDB_V2_ERROR_INPUT_INVALID;                                                                       \
		}                                                                                                               \
		DUCKDB_V2_ERROR rc = duckdb_v2_data_source_set_##what##_callback(source, (cb_type)callback, err);               \
		return rc != DUCKDB_V2_ERROR_NONE ? rc : duckdb_v2_copy_from_set_data_source(function, source, err);            \
	}

TABLE_SETTER(init_global, duckdb_v2_data_source_init_global_callback_fn)
TABLE_SETTER(init_local, duckdb_v2_data_source_init_local_callback_fn)
TABLE_SETTER(exec, duckdb_v2_data_source_exec_callback_fn)
TABLE_SETTER(progress, duckdb_v2_data_source_progress_callback_fn)
TABLE_SETTER(partition_data, duckdb_v2_data_source_partition_data_callback_fn)
TABLE_SETTER(claim_batch, duckdb_v2_data_source_claim_batch_callback_fn)
COPY_SETTER(init_global, duckdb_v2_data_source_init_global_callback_fn)
COPY_SETTER(init_local, duckdb_v2_data_source_init_local_callback_fn)
COPY_SETTER(exec, duckdb_v2_data_source_exec_callback_fn)
COPY_SETTER(progress, duckdb_v2_data_source_progress_callback_fn)
