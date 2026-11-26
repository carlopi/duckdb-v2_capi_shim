// duckdb_shim_extra/1 on this loader's DuckDB version: compiled against that version's own headers, resolved against
// the running DuckDB's exported symbols when the loader is loaded (-undefined dynamic_lookup), so no DuckDB code is
// linked in. Also the loading of children after the loader's own LOAD, through DuckDB's v2 entrypoint invoker.
#include "duckdb.hpp"
#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/main/capi_v2/extension_load_v2.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/extension_manager.hpp"
#include "duckdb/main/extension_install_info.hpp"
#include "duckdb/main/extension.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/file_system.hpp"

#include <cstring>
#include <string>

extern "C" {
#include "shim_extra.h"
}

using namespace duckdb;

#ifndef SHIM_CONTRACT_VERSION
#define SHIM_CONTRACT_VERSION "v2.0.1"
#endif
#ifndef DUCKDB_EXTENSION_NAME
#define DUCKDB_EXTENSION_NAME v2_capi_shim_v201
#endif
#define SHIM_STR2(x) #x
#define SHIM_STR(x) SHIM_STR2(x)
static const char *SHIM_CONTRACT = SHIM_CONTRACT_VERSION;
static const char *SHIM_LOADER = SHIM_STR(DUCKDB_EXTENSION_NAME);

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

//! Before a child is opened: its footer must say a v2 extension built for the contract on this platform (instead of
//! DuckDB's own check, which wants this DuckDB's version), and, unless allow_unsigned_extensions is set, it must be
//! signed by the named repository: core, community, or a user-provided one, checked by DuckDB's own code
bool shim_verify_child(struct _duckdb_v2_context *ctx, const char *path, const char *repo, const char *contract,
                       char *error, uint64_t error_capacity) {
	try {
		auto &context = ContextOf(ctx);
		auto &db = *context.db;
		auto &fs = FileSystem::GetFileSystem(context);
		auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
		auto meta = ExtensionHelper::ParseExtensionMetaData(*handle);
		std::string problem;
		if (!meta.AppearsValid()) {
			problem = "it has no DuckDB extension footer";
		} else if (meta.abi_type != ExtensionABIType::C_STRUCT_UNSTABLE) {
			problem = "it is not a v2 C API extension (footer ABI " + EnumUtil::ToString(meta.abi_type) + ")";
		} else if (meta.duckdb_version != contract) {
			problem = "it was built for DuckDB '" + meta.duckdb_version + "', this loader serves the '" + contract +
			          "' contract";
		} else if (meta.platform != DuckDB::Platform()) {
			problem = "it was built for platform '" + meta.platform + "', this is '" + DuckDB::Platform() + "'";
		}
		bool unsigned_ok = Settings::Get<AllowUnsignedExtensionsSetting>(db);
		if (!problem.empty() && !(unsigned_ok && Settings::Get<AllowExtensionsMetadataMismatchSetting>(db))) {
			SetError(error, error_capacity, problem);
			return false;
		}
		if (unsigned_ok) {
			return true;
		}
		std::string name = repo ? repo : "";
		if (name.empty()) {
			SetError(error, error_capacity,
			         "name the repository whose keys signed it: load_extension(path, repo := 'community') (or 'core', "
			         "or a repository created with CREATE REPOSITORY)");
			return false;
		}
		auto lower = StringUtil::Lower(name);
		auto type = lower == "core"        ? ExtensionRepositoryType::CORE
		            : lower == "community" ? ExtensionRepositoryType::COMMUNITY
		                                   : ExtensionRepositoryType::USER_PROVIDED;
		auto signing_name = type == ExtensionRepositoryType::USER_PROVIDED ? name : std::string();
		if (!ExtensionHelper::CheckExtensionSignature(db, *handle, meta, type, signing_name)) {
			SetError(error, error_capacity, "it is not signed by repository '" + name + "'");
			return false;
		}
		return true;
	} catch (std::exception &e) {
		SetError(error, error_capacity, ErrorData(e).RawMessage());
		return false;
	}
}

//! The description duckdb_extensions() shows for an extension (v2 has no call for it; this is what
//! ExtensionLoader::SetDescription does for a C++ extension)
static void Describe(DatabaseInstance &db, const std::string &name, const std::string &description) {
	auto info = ExtensionManager::Get(db).GetExtensionInfo(name);
	if (info) {
		auto loaded = make_uniq<ExtensionLoadedInfo>();
		loaded->description = description;
		info->load_info = std::move(loaded);
	}
}

void shim_describe_self(struct _duckdb_v2_context *ctx, const char *name, const char *contract) {
	try {
		auto &db = *ContextOf(ctx).db;
		Describe(db, name,
		         std::string("C API v2 bridge: runs v2 extensions built for DuckDB ") + contract + " on this DuckDB (" +
		             DuckDB::LibraryVersion() + ", " + DuckDB::SourceID() +
		             "), through the " + contract + " function table; offers the capabilities connections/1 and "
		             "interrupts/1. SELECT * FROM load_extension(path, repo := 'community') checks the footer and the "
		             "repository's signature, then loads");
	} catch (...) { // NOLINT: a description is not worth failing a load
	}
}

// set by the loader for the duration of one InvokeCAPIV2Entrypoint: the child entrypoint the trampoline forwards to
extern void shim_forward_child(::duckdb_v2_extension_input *input);

//! Loads a child after the loader's own LOAD: DuckDB's own v2 invoker makes the extension handle (registration works
//! through it as for any extension), the trampoline then hands the child the contract table instead of DuckDB's
bool shim_invoke_child(struct _duckdb_v2_context *ctx, const char *name, const char *path, void *lib, char *error,
                       uint64_t error_capacity) {
	try {
		auto &context = ContextOf(ctx);
		// recorded as loaded the way LOAD records an extension: duckdb_extensions() lists it, a second load is a no-op
		auto load = ExtensionManager::Get(*context.db).BeginLoad(ExtensionLoadOptions(name));
		if (!load) {
			return true;
		}
		ExtensionInitResult init;
		init.filename = name;
		init.filebase = name;
		init.abi_type = ExtensionABIType::C_STRUCT_UNSTABLE;
		init.lib_hdl = lib;
		try {
			InvokeCAPIV2Entrypoint(*context.db, init, name, shim_forward_child, &context, false);
		} catch (std::exception &ex) {
			load->LoadFail(ErrorData(ex));
			throw;
		}
		// the child's own version, from its footer
		std::string version = "unknown";
		try {
			auto handle = FileSystem::GetFileSystem(context).OpenFile(path, FileFlags::FILE_FLAGS_READ);
			auto meta = ExtensionHelper::ParseExtensionMetaData(*handle);
			if (meta.AppearsValid() && !meta.extension_version.empty()) {
				version = meta.extension_version;
			}
		} catch (...) { // NOLINT: a version is not worth failing a load
		}
		ExtensionInstallInfo install;
		install.mode = ExtensionInstallMode::NOT_INSTALLED;
		install.full_path = path;
		install.version = version;
		load->FinishLoad(install, path);
		Describe(*context.db, name, std::string("v2 extension built for DuckDB ") + SHIM_CONTRACT + ", loaded through " +
		                                SHIM_LOADER + " from " + path);
		return true;
	} catch (std::exception &e) {
		SetError(error, error_capacity, e.what());
		return false;
	}
}
}
