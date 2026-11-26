// duckdb_shim_extra/1 on this loader's DuckDB version: compiled against that version's own headers, resolved against
// the running DuckDB's exported symbols when the loader is loaded (-undefined dynamic_lookup), so no DuckDB code is
// linked in. Also the loading of children after the loader's own LOAD, through DuckDB's v2 entrypoint invoker.
#include "duckdb.hpp"
#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/main/capi_v2/extension_load_v2.hpp"
#include "duckdb/main/extension_helper.hpp"

#include <cstring>
#include <string>

extern "C" {
#include "shim_extra.h"
}

using namespace duckdb;

namespace {

ClientContext &ContextOf(struct _duckdb_v2_context *ctx) {
	return reinterpret_cast<capiv2::CV2Context *>(ctx)->context;
}

void SetError(char *error, uint64_t capacity, const std::string &message) {
	if (error && capacity > 0) {
		auto n = std::min<uint64_t>(capacity - 1, message.size());
		std::memcpy(error, message.data(), n);
		error[n] = '\0';
	}
}

bool ContextQuery(struct _duckdb_v2_context *ctx, const char *sql, uint64_t sql_length, duckdb_shim_row_fn row,
                  void *user_data, char *error, uint64_t error_capacity) {
	try {
		auto &context = ContextOf(ctx);
		Connection con(*context.db);
		auto result = con.Query(std::string(sql, sql_length));
		if (result->HasError()) {
			SetError(error, error_capacity, result->GetError());
			return false;
		}
		if (!row) {
			return true;
		}
		auto columns = result->ColumnCount();
		std::vector<std::string> text(columns);
		std::vector<const char *> values(columns);
		std::vector<uint64_t> lengths(columns);
		while (auto chunk = result->Fetch()) {
			for (idx_t r = 0; r < chunk->size(); r++) {
				for (idx_t c = 0; c < columns; c++) {
					auto v = chunk->GetValue(c, r);
					if (v.IsNull()) {
						values[c] = nullptr;
						lengths[c] = 0;
					} else {
						text[c] = v.ToString();
						values[c] = text[c].c_str();
						lengths[c] = text[c].size();
					}
				}
				row(user_data, columns, values.data(), lengths.data());
			}
		}
		return true;
	} catch (std::exception &e) {
		SetError(error, error_capacity, e.what());
		return false;
	}
}

//! Ctrl-C and max_execution_time alike: InterruptCheck throws on either (IsInterrupted only sees the flag)
bool ContextInterrupted(struct _duckdb_v2_context *ctx) {
	try {
		ContextOf(ctx).InterruptCheck();
		return false;
	} catch (...) {
		return true;
	}
}

const duckdb_shim_connections_v1 CONNECTIONS = {ContextQuery};
const duckdb_shim_interrupts_v1 INTERRUPTS = {ContextInterrupted};

} // namespace

extern "C" {

//! Offers every capability this loader has to a child that asks for them
void shim_offer_all(duckdb_shim_offer_fn offer) {
	offer(DUCKDB_SHIM_CONNECTIONS_V1, &CONNECTIONS);
	offer(DUCKDB_SHIM_INTERRUPTS_V1, &INTERRUPTS);
}

// set by the loader for the duration of one InvokeCAPIV2Entrypoint: the child entrypoint the trampoline forwards to
extern void shim_forward_child(::duckdb_v2_extension_input *input);

//! Loads a child after the loader's own LOAD: DuckDB's own v2 invoker makes the extension handle (registration works
//! through it as for any extension), the trampoline then hands the child the contract table instead of DuckDB's
bool shim_invoke_child(struct _duckdb_v2_context *ctx, const char *name, void *lib, char *error,
                       uint64_t error_capacity) {
	try {
		auto &context = ContextOf(ctx);
		ExtensionInitResult init;
		init.filename = name;
		init.filebase = name;
		init.abi_type = ExtensionABIType::C_STRUCT_UNSTABLE;
		init.lib_hdl = lib;
		InvokeCAPIV2Entrypoint(*context.db, init, name, shim_forward_child, &context, false);
		return true;
	} catch (std::exception &e) {
		SetError(error, error_capacity, e.what());
		return false;
	}
}
}
