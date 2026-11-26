//===--------------------------------------------------------------------===//
// v2_capi_shim: runs extensions built against the v2 C API of one DuckDB release (the contract) on the DuckDB it is
// built with (the target), by handing them the contract's function table implemented on the target's.
//
//   load_extension(path, repo := ...)          checks a file (footer, the repository's signature), then loads it
//   install_and_load_extension(name, repo)     fetches the contract build from a repository, checks it, stores it, loads
//   v2_capi_shim_info()                        contract, target, platform
//   v2_capi_shim_capabilities()                the named function tables offered beyond the contract
//
// Built by DuckDB's own extension build: duckdb_extension_load(v2_capi_shim SOURCE_DIR <this repository>). The
// adapters (shim.c) and the contract table (compat_table.c) are generated at configure time from this DuckDB's
// duckdb_v2.h and the contract's (contracts/<version>), with python3 only.
//===--------------------------------------------------------------------===//
#define DUCKDB_EXTENSION_MAIN

#include "v2_capi_shim_extension.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/gzip_file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/main/capi_v2/extension_load_v2.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/extension.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/extension_install_info.hpp"
#include "duckdb/main/extension_manager.hpp"
#include "duckdb/main/extension_repository_manager.hpp"
#include "duckdb/main/http/http_transport_manager.hpp"
#include "duckdb/main/http/http_util.hpp"
#include "duckdb/main/settings.hpp"

#include <cstring>
#include <dlfcn.h>

extern "C" {
#include "duckdb_v2.h"
#include "shim_extra.h"
#include "shim_target.h"
//! the contract-layout table (compat_table.c, generated)
extern const char compat_table[];
//! src/target_api.c: the running DuckDB's table, for the adapters
void shim_set_target_api(const void *api);
}

#ifndef V2_CAPI_SHIM_CONTRACT
#define V2_CAPI_SHIM_CONTRACT "v2.0.1"
#endif

namespace duckdb {

namespace {

constexpr const char *CONTRACT = V2_CAPI_SHIM_CONTRACT;
constexpr const char *LOADER = "v2_capi_shim";

//===--------------------------------------------------------------------===//
// Capabilities: named function tables beyond the contract, offered to a child that exports
// <name>_duckdb_shim_extra(capability, table); DuckDB itself never calls that, so natively they stay absent
//===--------------------------------------------------------------------===//

ClientContext &ContextOf(struct _duckdb_v2_context *ctx) {
	return reinterpret_cast<capiv2::CV2Context *>(ctx)->context;
}

void SetError(char *error, uint64_t capacity, const string &message) {
	if (error && capacity > 0) {
		auto n = MinValue<uint64_t>(capacity - 1, message.size());
		std::memcpy(error, message.data(), n);
		error[n] = '\0';
	}
}

bool ContextQuery(struct _duckdb_v2_context *ctx, const char *sql, uint64_t sql_length, duckdb_shim_row_fn row,
                  void *user_data, char *error, uint64_t error_capacity) {
	try {
		auto &context = ContextOf(ctx);
		Connection con(*context.db);
		auto result = con.Query(string(sql, sql_length));
		if (result->HasError()) {
			SetError(error, error_capacity, result->GetError());
			return false;
		}
		if (!row) {
			return true;
		}
		auto columns = result->ColumnCount();
		vector<string> text(columns);
		vector<const char *> values(columns);
		vector<uint64_t> lengths(columns);
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

struct Capability {
	const char *name;
	const char *version;
	const char *functions;
	const char *description;
	const void *table;
};
const Capability CAPABILITIES[] = {
    {"connections", "1", "context_query",
     "SQL on a connection of its own to the database of a callback's context (offered as 'connections/1')",
     &CONNECTIONS},
    {"interrupts", "1", "context_interrupted",
     "whether the query of a context was interrupted: Ctrl-C, max_execution_time (offered as 'interrupts/1')",
     &INTERRUPTS},
};

//===--------------------------------------------------------------------===//
// Checking a child: its footer must say a v2 extension built for the contract on this platform (in place of DuckDB's
// check for its own version), and, unless allow_unsigned_extensions is set, it must be signed by the named repository
//===--------------------------------------------------------------------===//

string FooterProblem(ParsedExtensionMetaData &meta) {
	if (!meta.AppearsValid()) {
		return "it has no DuckDB extension footer";
	}
	if (meta.abi_type != ExtensionABIType::C_STRUCT_UNSTABLE) {
		return "it is not a v2 C API extension (footer ABI " + EnumUtil::ToString(meta.abi_type) + ")";
	}
	if (meta.duckdb_version != CONTRACT) {
		return "it was built for DuckDB '" + meta.duckdb_version + "', this loader serves the '" + string(CONTRACT) +
		       "' contract";
	}
	if (meta.platform != DuckDB::Platform()) {
		return "it was built for platform '" + meta.platform + "', this is '" + DuckDB::Platform() + "'";
	}
	return string();
}

void RepositoryKeys(const string &name, ExtensionRepositoryType &type, string &signing_name) {
	auto lower = StringUtil::Lower(name);
	type = lower == "core" ? ExtensionRepositoryType::CORE
	       : lower == "community" ? ExtensionRepositoryType::COMMUNITY
	                              : ExtensionRepositoryType::USER_PROVIDED;
	signing_name = type == ExtensionRepositoryType::USER_PROVIDED ? name : string();
}

bool MetadataMismatchAllowed(DatabaseInstance &db) {
	return Settings::Get<AllowUnsignedExtensionsSetting>(db) && Settings::Get<AllowExtensionsMetadataMismatchSetting>(db);
}

void VerifyChild(ClientContext &context, const string &path, const string &repo) {
	auto &db = *context.db;
	auto handle = FileSystem::GetFileSystem(context).OpenFile(path, FileFlags::FILE_FLAGS_READ);
	auto meta = ExtensionHelper::ParseExtensionMetaData(*handle);
	auto problem = FooterProblem(meta);
	if (!problem.empty() && !MetadataMismatchAllowed(db)) {
		throw InvalidInputException("v2_capi_shim: refusing %s: %s", path, problem);
	}
	if (Settings::Get<AllowUnsignedExtensionsSetting>(db)) {
		return;
	}
	if (repo.empty()) {
		throw InvalidInputException("v2_capi_shim: refusing %s: name the repository whose keys signed it: "
		                            "load_extension(path, repo := 'community') (or 'core', or a repository created "
		                            "with CREATE REPOSITORY)",
		                            path);
	}
	ExtensionRepositoryType type;
	string signing_name;
	RepositoryKeys(repo, type, signing_name);
	if (!ExtensionHelper::CheckExtensionSignature(db, *handle, meta, type, signing_name)) {
		throw InvalidInputException("v2_capi_shim: refusing %s: it is not signed by repository '%s'", path, repo);
	}
}

//===--------------------------------------------------------------------===//
// Loading a child: DuckDB's own v2 invoker makes the extension handle (registration works through it as for any
// extension); the trampoline it calls fetches DuckDB's table on the child's behalf (DuckDB checks that the extension
// did), offers the capabilities, and calls the child with the contract's table instead
//===--------------------------------------------------------------------===//

typedef void (*child_init_fn)(::duckdb_v2_extension_input *);
struct PendingChild {
	child_init_fn init = nullptr;
	void *lib = nullptr;
	string name;
	bool fetched = false;
};
thread_local PendingChild *pending = nullptr;

//! get_api as the contract declares it, (extension, version): like DuckDB's own for a version-pinned extension, the
//! name is the build's identity (a tag, a commit, '2.0.0'), not a request; the footer chose the contract
const void *ContractGetApi(duckdb_v2_extension_handle, const char *) {
	if (pending) {
		pending->fetched = true;
	}
	return compat_table;
}

extern "C" void ShimForwardChild(::duckdb_v2_extension_input *input) {
	auto &child = *pending;
	// the running DuckDB's table: fetching it also tells DuckDB the extension did, and the adapters call through it
	const void *target_api = nullptr;
	if (input->get_api) {
#if SHIM_TARGET_GET_API_HAS_LENGTH
		target_api = input->get_api(input->extension, CONTRACT, strlen(CONTRACT));
#else
		target_api = input->get_api(input->extension, CONTRACT);
#endif
		if (!target_api) {
			return; // DuckDB recorded why
		}
		shim_set_target_api(target_api);
	}
	auto offer = reinterpret_cast<duckdb_shim_offer_fn>(dlsym(child.lib, (child.name + "_duckdb_shim_extra").c_str()));
	if (offer) {
		offer(DUCKDB_SHIM_CONNECTIONS_V1, &CONNECTIONS);
		offer(DUCKDB_SHIM_INTERRUPTS_V1, &INTERRUPTS);
	}
	auto contract = *input; // same layout; only get_api's signature differs
	contract.get_api = reinterpret_cast<duckdb_v2_extension_get_api_fn>(reinterpret_cast<void (*)()>(ContractGetApi));
	child.fetched = false;
	child.init(&contract);
	if (!child.fetched) {
		throw InvalidInputException("v2_capi_shim: extension '%s' did not fetch its API table "
		                            "(DUCKDB_EXTENSION_API_INIT): refusing it",
		                            child.name);
	}
}

void Describe(DatabaseInstance &db, const string &name, const string &description) {
	auto info = ExtensionManager::Get(db).GetExtensionInfo(name);
	if (info) {
		auto loaded = make_uniq<ExtensionLoadedInfo>();
		loaded->description = description;
		info->load_info = std::move(loaded);
	}
}

//! Checks, opens and initializes a child; returns its name
string LoadChild(ClientContext &context, const string &path, const string &repo) {
	VerifyChild(context, path, repo);
	auto &fs = FileSystem::GetFileSystem(context);
	auto base = fs.ExtractBaseName(path);
	auto name = base.substr(0, base.find('.'));
	auto load = ExtensionManager::Get(*context.db).BeginLoad(ExtensionLoadOptions(name));
	if (!load) {
		return name; // already loaded
	}
	try {
		void *lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (!lib) {
			throw IOException("v2_capi_shim: cannot open %s: %s", path, dlerror());
		}
		auto init = reinterpret_cast<child_init_fn>(dlsym(lib, (name + "_init_c_api_v2").c_str()));
		if (!init) {
			throw IOException("v2_capi_shim: %s has no entrypoint %s_init_c_api_v2", path, name);
		}
		PendingChild child;
		child.init = init;
		child.lib = lib;
		child.name = name;
		pending = &child;
		ExtensionInitResult init_result;
		init_result.filename = path;
		init_result.filebase = name;
		init_result.abi_type = ExtensionABIType::C_STRUCT_UNSTABLE;
		init_result.lib_hdl = lib;
		try {
			// through the running DuckDB's own invoker (its database holds it): it hands out that DuckDB's table and
			// handles, so the child's calls run the running DuckDB's code, not this extension's copy of it
			context.db->InvokeExtensionEntrypointV2(init_result, name, ShimForwardChild, &context, false);
		} catch (...) {
			pending = nullptr;
			throw;
		}
		pending = nullptr;
	} catch (std::exception &ex) {
		load->LoadFail(ErrorData(ex));
		throw;
	}
	string version = "unknown";
	{
		auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
		auto meta = ExtensionHelper::ParseExtensionMetaData(*handle);
		if (meta.AppearsValid() && !meta.extension_version.empty()) {
			version = meta.extension_version;
		}
	}
	ExtensionInstallInfo install;
	install.mode = ExtensionInstallMode::NOT_INSTALLED;
	install.full_path = path;
	install.version = version;
	load->FinishLoad(install, path);
	Describe(*context.db, name,
	         "v2 extension built for DuckDB " + string(CONTRACT) + ", loaded through " + LOADER + " from " + path);
	return name;
}

//===--------------------------------------------------------------------===//
// Installing: the repository name resolved to its location the way INSTALL resolves it, the contract's build fetched
// with DuckDB's HTTP client, its footer and signature checked in memory (only checked bytes are written), stored under
// <extension directory>/v2_capi_shim/<contract>/<name>.v2_capi_shim (not *.duckdb_extension: only INSTALL writes
// those, and DuckDB's own LOAD should not pick these up)
//===--------------------------------------------------------------------===//

string InstallChild(ClientContext &context, const string &name, const string &repo) {
	auto &db = *context.db;
	auto &fs = FileSystem::GetFileSystem(context);
	ExtensionRepository repository;
	if (!ExtensionRepository::TryGetKnownRepository(repo, repository) &&
	    !ExtensionRepositoryManager::TryGetRepository(db, fs, repo, repository)) {
		throw InvalidInputException(
		    "v2_capi_shim: unknown repository '%s': core, community, or a repository created with CREATE REPOSITORY",
		    repo);
	}
	auto url = repository.path + "/" + CONTRACT + "/" + DuckDB::Platform() + "/" + name + ".duckdb_extension.gz";
	auto session = db.config.GetHTTPTransportManager().CreateSession(context, url);
	HTTPHeaders headers;
	GetRequestInfo request(url, headers, session.Parameters(), nullptr, nullptr);
	auto response = session.Request(request);
	if (!response->Success()) {
		throw IOException("v2_capi_shim: could not fetch %s: %s", url,
		                  response->HasRequestError() ? response->GetRequestError()
		                                              : "HTTP " + std::to_string(int(response->status)));
	}
	auto bytes = GZipFileSystem::UncompressGZIPString(response->body);
	if (bytes.size() < ParsedExtensionMetaData::FOOTER_SIZE) {
		throw IOException("v2_capi_shim: %s is too small to be an extension", url);
	}
	auto meta = ExtensionHelper::ParseExtensionMetaData(bytes.data() + bytes.size() - ParsedExtensionMetaData::FOOTER_SIZE);
	auto problem = FooterProblem(meta);
	if (!problem.empty() && !MetadataMismatchAllowed(db)) {
		throw InvalidInputException("v2_capi_shim: refusing %s: %s", url, problem);
	}
	if (!Settings::Get<AllowUnsignedExtensionsSetting>(db)) {
		ExtensionRepositoryType type;
		string signing_name;
		RepositoryKeys(repo, type, signing_name);
		if (!ExtensionHelper::CheckExtensionBufferSignature(db, bytes.data(), bytes.size(), type, signing_name,
		                                                    nullptr)) {
			throw InvalidInputException("v2_capi_shim: refusing %s: it is not signed by repository '%s'", url, repo);
		}
	}
	auto directories = ExtensionHelper::GetExtensionDirectoryPath(context);
	if (directories.empty()) {
		throw IOException("v2_capi_shim: no extension directory to install into");
	}
	auto dir = fs.JoinPath(fs.JoinPath(directories[0], LOADER), CONTRACT);
	fs.CreateDirectoriesRecursive(dir);
	auto path = fs.JoinPath(dir, name + ".v2_capi_shim");
	auto tmp = path + ".tmp";
	{
		auto out = fs.OpenFile(tmp, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
		out->Write(const_cast<char *>(bytes.data()), bytes.size());
		out->Sync();
	}
	fs.MoveFile(tmp, path);
	return path;
}

//===--------------------------------------------------------------------===//
// Table functions
//===--------------------------------------------------------------------===//

struct RowsBindData : public TableFunctionData {
	vector<vector<Value>> rows;
};

struct RowsState : public GlobalTableFunctionState {
	idx_t next = 0;
};

unique_ptr<GlobalTableFunctionState> RowsInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<RowsState>();
}

void RowsFun(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<RowsBindData>();
	auto &state = data_p.global_state->Cast<RowsState>();
	idx_t count = 0;
	while (state.next < bind.rows.size() && count < STANDARD_VECTOR_SIZE) {
		for (idx_t c = 0; c < bind.rows[state.next].size(); c++) {
			output.data[c].SetValue(count, bind.rows[state.next][c]);
		}
		state.next++;
		count++;
	}
	output.SetCardinality(count);
}

string NamedText(TableFunctionBindInput &input, const string &name) {
	for (auto &kv : input.named_parameters) {
		if (StringUtil::CIEquals(kv.first.GetIdentifierName(), name) && !kv.second.IsNull()) {
			return kv.second.ToString();
		}
	}
	return string();
}

unique_ptr<FunctionData> LoadBind(ClientContext &context, TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<RowsBindData>();
	result->rows.push_back({Value(LoadChild(context, input.inputs[0].ToString(), NamedText(input, "repo")))});
	names.emplace_back("loaded");
	return_types.emplace_back(LogicalType::VARCHAR);
	return std::move(result);
}

unique_ptr<FunctionData> InstallBind(ClientContext &context, TableFunctionBindInput &input,
                                     vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto name = input.inputs[0].ToString();
	auto repo = input.inputs[1].ToString();
	auto path = InstallChild(context, name, repo);
	auto result = make_uniq<RowsBindData>();
	result->rows.push_back({Value(LoadChild(context, path, repo))});
	names.emplace_back("loaded");
	return_types.emplace_back(LogicalType::VARCHAR);
	return std::move(result);
}

unique_ptr<FunctionData> InfoBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                  vector<Identifier> &names) {
	for (auto col : {"contract", "target_version", "target_source_id", "platform", "kind"}) {
		names.emplace_back(col);
		return_types.emplace_back(LogicalType::VARCHAR);
	}
	auto result = make_uniq<RowsBindData>();
	result->rows.push_back({Value(CONTRACT), Value(DuckDB::LibraryVersion()), Value(DuckDB::SourceID()),
	                        Value(DuckDB::Platform()), Value("bridge+capabilities")});
	return std::move(result);
}

unique_ptr<FunctionData> CapabilitiesBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                          vector<Identifier> &names) {
	for (auto col : {"name", "version", "functions", "description"}) {
		names.emplace_back(col);
		return_types.emplace_back(LogicalType::VARCHAR);
	}
	auto result = make_uniq<RowsBindData>();
	for (auto &c : CAPABILITIES) {
		result->rows.push_back({Value(c.name), Value(c.version), Value(c.functions), Value(c.description)});
	}
	return std::move(result);
}

} // namespace

static void LoadInternal(ExtensionLoader &loader) {
	loader.SetDescription("C API v2 bridge: runs v2 extensions built for DuckDB " + string(CONTRACT) +
	                      " on this DuckDB, through the " + string(CONTRACT) +
	                      " function table; offers the capabilities connections/1 and interrupts/1. "
	                      "load_extension(path, repo := 'community') checks the footer and the repository's "
	                      "signature, then loads; install_and_load_extension(name, repo) fetches it first");
	TableFunction load("load_extension",
	                   FunctionSignature()
	                       .AddPositionalOnly("path", LogicalType::VARCHAR)
	                       .AddKeywordOnly("repo", LogicalType::VARCHAR, Value(LogicalType::VARCHAR)),
	                   RowsFun, LoadBind, RowsInit);
	loader.RegisterFunction(load);
	loader.RegisterFunction(TableFunction("install_and_load_extension", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                      RowsFun, InstallBind, RowsInit));
	loader.RegisterFunction(TableFunction("v2_capi_shim_info", {}, RowsFun, InfoBind, RowsInit));
	loader.RegisterFunction(TableFunction("v2_capi_shim_capabilities", {}, RowsFun, CapabilitiesBind, RowsInit));
}

void V2CapiShimExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string V2CapiShimExtension::Name() {
	return "v2_capi_shim";
}

std::string V2CapiShimExtension::Version() const {
#ifdef EXT_VERSION_V2_CAPI_SHIM
	return EXT_VERSION_V2_CAPI_SHIM;
#else
	return string(CONTRACT) + "-on-" + DuckDB::SourceID();
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(v2_capi_shim, loader) {
	duckdb::LoadInternal(loader);
}
}
