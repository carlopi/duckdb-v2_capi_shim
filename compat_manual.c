/* Hand-written: the log sink. In v2.0.1 the write callback takes the message as duckdb_v2_str; in main as (ptr, len).
 * The callback has no context of its own besides the sink's user data, which the flush callback shares, so the shim
 * owns the user data slot: one record per sink, installed once, holding the extension's callbacks and its user data. */
#include <pthread.h>
#include <stdlib.h>
#include "duckdb_v2.h"
typedef struct compat_v2_str { const char *ptr; idx_t len; } compat_v2_str;
typedef void (*compat_v2_log_sink_write_callback_fn)(duckdb_v2_log_sink_write_info_handle, void *, int64_t,
                                                     DUCKDB_V2_LOG_LEVEL, DUCKDB_V2_LOG_SCOPE, const compat_v2_str *,
                                                     const compat_v2_str *, duckdb_v2_error_info_handle *);
typedef struct sink_rec {
	duckdb_v2_log_sink_handle sink;
	compat_v2_log_sink_write_callback_fn write;
	duckdb_v2_log_sink_flush_callback_fn flush;
	duckdb_v2_opaque user;
	struct sink_rec *next;
} sink_rec;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static sink_rec *recs;

static void rec_free(void *p) {
	sink_rec *r = (sink_rec *)p;
	pthread_mutex_lock(&lock);
	for (sink_rec **it = &recs; *it; it = &(*it)->next) {
		if (*it == r) { *it = r->next; break; }
	}
	pthread_mutex_unlock(&lock);
	if (r->user.destroy) r->user.destroy(r->user.ptr);
	free(r);
}
static void write_tramp(duckdb_v2_log_sink_write_info_handle info, void *ud, int64_t ts, DUCKDB_V2_LOG_LEVEL level,
                        DUCKDB_V2_LOG_SCOPE scope, const char *type, idx_t type_len, const char *msg, idx_t msg_len,
                        duckdb_v2_error_info_handle *err) {
	sink_rec *r = (sink_rec *)ud;
	compat_v2_str t = {type, type_len}, m = {msg, msg_len};
	if (r->write) r->write(info, r->user.ptr, ts, level, scope, &t, &m, err);
}
static void flush_tramp(duckdb_v2_log_sink_flush_info_handle info, void *ud, duckdb_v2_error_info_handle *err) {
	sink_rec *r = (sink_rec *)ud;
	if (r->flush) r->flush(info, r->user.ptr, err);
}
/* the record of a sink, created and installed as the sink's user data on first use */
static sink_rec *rec_of(duckdb_v2_log_sink_handle sink, duckdb_v2_error_info_handle *err, DUCKDB_V2_ERROR *rc) {
	pthread_mutex_lock(&lock);
	sink_rec *r = recs;
	while (r && r->sink != sink) r = r->next;
	if (!r) {
		r = (sink_rec *)calloc(1, sizeof(sink_rec));
		r->sink = sink;
		r->next = recs;
		recs = r;
		pthread_mutex_unlock(&lock);
		duckdb_v2_opaque o = {r, rec_free, NULL};
		*rc = duckdb_v2_log_sink_set_user_data(sink, &o, err);
		duckdb_v2_log_sink_set_write_callback(sink, write_tramp, NULL);
		duckdb_v2_log_sink_set_flush_callback(sink, flush_tramp, NULL);
		return r;
	}
	pthread_mutex_unlock(&lock);
	*rc = (DUCKDB_V2_ERROR)0;
	return r;
}
DUCKDB_V2_ERROR compat_duckdb_v2_log_sink_set_write_callback(duckdb_v2_log_sink_handle sink,
                                                             compat_v2_log_sink_write_callback_fn cb,
                                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_V2_ERROR rc;
	rec_of(sink, err, &rc)->write = cb;
	return rc;
}
DUCKDB_V2_ERROR compat_duckdb_v2_log_sink_set_flush_callback(duckdb_v2_log_sink_handle sink,
                                                             duckdb_v2_log_sink_flush_callback_fn cb,
                                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_V2_ERROR rc;
	rec_of(sink, err, &rc)->flush = cb;
	return rc;
}
DUCKDB_V2_ERROR compat_duckdb_v2_log_sink_set_user_data(duckdb_v2_log_sink_handle sink, duckdb_v2_opaque *data,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_V2_ERROR rc;
	sink_rec *r = rec_of(sink, err, &rc);
	if (r->user.destroy) r->user.destroy(r->user.ptr);
	r->user = data ? *data : (duckdb_v2_opaque){0};
	return rc;
}
