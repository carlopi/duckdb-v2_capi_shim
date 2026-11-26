/* A v2 extension built against the v2.0.1 contract headers. It goes through adapted functions on purpose: names and
 * bodies are string structs in v2.0.1, (ptr, len) pairs in main. */
#define DUCKDB_EXTENSION_NAME child
#include "duckdb_extension_v2.h"
DUCKDB_EXTENSION_EXTERN
#include <string.h>

static duckdb_v2_str S(const char *s) {
	duckdb_v2_str r = {s, strlen(s)};
	return r;
}

static int fail(duckdb_v2_error_info_handle *err, duckdb_v2_error_info_handle local) {
	if (local) {
		duckdb_v2_str text = {0, 0};
		duckdb_v2_error_info_get_text(local, &text);
		duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_error_info_set_text(*err, &text);
		duckdb_v2_error_info_destroy(&local);
		return 1;
	}
	return 0;
}

static int macro(duckdb_v2_extension_handle ext, duckdb_v2_factory_handle factory, DUCKDB_V2_MACRO_FUNCTION_TYPE type,
                 const char *name, const char *param, const char *body, duckdb_v2_error_info_handle *err) {
	duckdb_v2_error_info_handle e = NULL;
	duckdb_v2_macro_function_handle fn = NULL;
	duckdb_v2_function_signature_handle sig = NULL;
	duckdb_v2_logical_type_handle varchar = NULL;
	duckdb_v2_str n = S(name), p = S(param), b = S(body);
	duckdb_v2_macro_function_create(factory, type, &fn, &e);
	if (!e) duckdb_v2_macro_function_set_name(fn, &n, &e);
	if (!e) duckdb_v2_macro_function_set_body(fn, &b, &e);
	if (!e) duckdb_v2_macro_function_get_signature(fn, &sig, &e);
	if (!e) duckdb_v2_factory_create_type_from_id(factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR, NULL, NULL, 0, &varchar, &e);
	if (!e) duckdb_v2_function_signature_add_parameter(sig, &p, varchar, NULL, DUCKDB_V2_FUNCTION_PARAMETER_KIND_STANDARD, &e);
	if (!e) duckdb_v2_extension_register_macro_function(ext, fn, &e);
	duckdb_v2_logical_type_destroy(&varchar);
	duckdb_v2_macro_function_destroy(&fn);
	return fail(err, e);
}

DUCKDB_EXTENSION_ENTRYPOINT(duckdb_v2_extension_handle extension, duckdb_v2_context_handle context,
                            duckdb_v2_error_info_handle *err) {
	duckdb_v2_error_info_handle e = NULL;
	duckdb_v2_factory_handle factory = NULL;
	duckdb_v2_context_get_factory(context, &factory, &e);
	if (fail(err, e)) return;
	if (macro(extension, factory, DUCKDB_V2_MACRO_FUNCTION_TYPE_SCALAR, "child_greet", "who",
	          "'hello ' || who || ', from an extension built for v2.0.1'", err)) return;
	macro(extension, factory, DUCKDB_V2_MACRO_FUNCTION_TYPE_TABLE, "child_letters", "word",
	      "SELECT unnest(string_split(word, '')) AS letter", err);
}
