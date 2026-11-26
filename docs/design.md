# Spike: is the v2 C API of v2.0.1 servable on a later DuckDB?

> Written during the spike inside duckdb-container; paths like `tools/capi-compat/...` are now `spike/...` (scripts)
> or the top level (generator), and the loader is the CMake-built extension in `src/`.

Question: can an extension built once against the v2 C API of DuckDB v2.0.1 (the *contract*) run on a later DuckDB
(the *target*, here upstream `main` at `c826f8f`, 2026-11-26) through a compat layer, instead of being rebuilt per
version? Reproduce with `tools/capi-compat/run.sh [git-ref]`.

## Verdict

Yes, for this pair, at compile and link level. A generated compat object (169 KB) exports a complete v2.0.1-layout
`duckdb_ext_api_v2` table (641 slots) implemented on top of the target's v2 functions; it compiles against the
target's headers with `-Werror -Wincompatible-pointer-types -Wconversion` and links with no unresolved adapter. Not
yet run: that needs a DuckDB built from the target.

## The difference between the two surfaces (clang AST of both `duckdb_v2.h`)

| | contract | target | identical | changed | removed | added |
|---|---|---|---|---|---|---|
| functions | 641 | 728 | 540 | 101 | 0 | 87 |
| typedefs | 177 | 203 | 171 | 4 | 2 | 28 |
| structs | 103 | 110 | 101 | 1 (layout identical) | 1 | 8 |
| enum values | 249 | 278 | 249 | 0 | 0 | 29 |

Almost every change is one systematic edit: the string struct `duckdb_v2_str` / `duckdb_v2_identifier_t` (`{ptr, len}`)
is gone, replaced by `(const char *, idx_t)` pairs, and arrays of it by parallel `(const char *const *, const idx_t *)`
arrays. The same edit hit four callback typedefs (text sink, glob callback, log sink write, `get_api`).

## How each contract function is served

| class | count | how |
|---|---|---|
| forward | 537 | identical signature: call through |
| generated | 101 | rewrite rules (below) |
| hand-written | 3 | `compat_manual.c` |
| impossible | 0 | |

Rewrite rules, each mechanical and generated from the two ASTs:

1. string struct in (const or not) -> `(ptr, len)`
2. string struct out -> two temporaries, repacked after the call
3. array of string structs + count -> two temporary parallel arrays
4. NUL-terminated `const char *` that gained a length -> `strlen`
5. callback taking a string struct, invoked during the call (text sink, glob) -> trampoline with a stack context
6. integer widened (decimal width/scale `uint8_t` -> `uint32_t`) -> cast in; narrow out (values <= 38)
7. pointer that gained `const` -> forward

Hand-written: the log sink. Its write callback is stored, not called during the call, and has no context of its own
except the sink's user data, which the flush callback shares. So the adapter owns the user-data slot (one record per
sink, holding the extension's callbacks and user data, freed through the opaque's destructor) and wraps
`set_user_data`, `set_write_callback` and `set_flush_callback` together. The compiler flagged the write callback; it
could not have flagged the other two, which kept their signatures: shared state is where generated adapters stop.

## What signatures do not show (spec diff of the 537 identical functions)

25 of them changed in `api_spec/v2`: 21 only in `lifecycle` (e.g. the stats functions went from `stable` back to
`unstable`), 7 in their description. Some descriptions are behaviour changes behind an unchanged signature:

- `table_function_set_filter_pushdown_callback`: a function that also has projection pushdown is now offered its
  single-column predicates a second time when a scan starts
- `statement_bind`: errors of planning after the bind now surface at a different step
- `connection_get_query_metrics`: "the last query profiled", no longer "the last query finished"
- `logical_type_get_param_count`: GEOGRAPHY added next to GEOMETRY

No adapter can see these; they need a per-function decision (accept, adapt, or forbid) and tests run per
extension/target pair.

## Shape this points to

- **Contract**: a frozen v2 snapshot (v2.0.1 to start with), optionally minus a denylist: functions that are hard to
  serve (stateful like the log sink, or changed behaviour) are left out of the contract header, so extensions cannot
  call them, and their slots point at a stub that fails.
- **Compat component per target Z**: serves one or more contracts (a bounded window) by handing out the contract's
  table from `get_api`, built as here. Extensions are built once per contract: N extensions + M targets instead of
  N x M builds, and no `libduckdb_static.a` anywhere.
- **Where it lives**: best in DuckDB's loader. An extension whose footer says contract v2.0.1 is loaded on Z with the
  v2.0.1 table, keeping `INSTALL`/`LOAD`, autoloading, repositories and signature checks. As a separate extension
  (`LOAD v2_capi_shim; CALL load_extension('...')`) it hits a gap: registering functions needs a
  `duckdb_v2_extension_handle`, which is the load info DuckDB passes to an entrypoint and is only valid during it, and
  v2 has no call that loads an extension from a function. Such a shim could only load its children during its own
  `LOAD`; anything later needs one new v2 function (load an extension through a provided `get_api`, or register an
  API provider with the loader).

## Runtime proof

`tools/capi-compat/runtime/`: `child.c` is a v2 extension built against the v2.0.1 headers. It registers a scalar
macro and a table macro through adapted functions (names, bodies and parameter names are string structs in v2.0.1).
`shim_entry.c` is `v2_capi_shim`, a v2 extension built against main's headers: it contains the compat object, and its
entrypoint `dlopen`s the child named by `V2_CAPI_SHIM_LOAD` and calls the child's `child_init_c_api_v2` with a
contract-layout input (DuckDB's own extension handle, context and error slot, and a `get_api` returning the v2.0.1
table). Build with `tools/capi-compat/run.sh && TARGET_VERSION=<id> tools/capi-compat/runtime/build.sh`.

The same child binary, on DuckDB main (`c826f8f715`, a release build of `.cache/duckdb-main`):

    V2_CAPI_SHIM_LOAD=child.duckdb_extension duckdb -unsigned -c "LOAD 'v2_capi_shim.duckdb_extension'; ..."
    child_greet('main')          -> hello main, from an extension built for v2.0.1
    child_letters('duck')        -> d, u, c, k
    duckdb_functions()           -> child_greet (macro), child_letters (table_macro)

Loaded directly on main (stamped v2.0.1), it is refused: built for v2.0.1 only. Stamped v2.0.1 and loaded on
v2.0.1, it works natively. Shim 156 KB, child 34 KB, no DuckDB code in either.

The shim's footer must carry the target's exact version id (a dev build reports its commit, `c826f8f715`). Children
loaded through it have no footer checked at all: in a real loader, the contract version in the child's footer is what
selects the table, and signatures need checking as for any extension.

A real one too: the container extension's v2 proof (`container_engine()`, `containers()`, the `container_engine`
option; built for v2.0.1 by spike 2, `docs/spikes/capi-v2-port.md` on its branch), the same 203 KB binary, on main
through the bridge: the engine answers, `containers()` returns its 22 rows, `SET container_engine` is read back
through `config_get_option_value` (an adapted function: its name went from a string struct to pointer and length),
and a bad socket fails the query with the extension's own message.

## Checking what a loader loads

`load_extension(path, repo := 'name')` (the loader with capabilities) checks a file before opening it, with DuckDB's
own code (it carries DuckDB statically anyway): the footer, parsed by `ExtensionHelper::ParseExtensionMetaData`, must
say a v2 extension (`C_STRUCT_UNSTABLE`) built for the contract (v2.0.1) on this platform, in place of DuckDB's check
for its own version; and unless `allow_unsigned_extensions` is set, `repo` is required and the signature is verified
by `ExtensionHelper::CheckExtensionSignature` against that repository's keys (`core`, `community`, or a user-provided
repository by name). Refused, each with its reason: no footer; a `CPP` footer (even with unsigned allowed); a signed-by-
nobody v2.0.1 file without `repo`, and with `repo := 'community'`.

## A community extension on main, signature checked (in Docker)

`tools/capi-compat/extras/docker_run.sh`: DuckDB main built for linux_arm64 in a container, the loader built there
(linked with that build's `libduckdb_static.a`, exporting only its entrypoint; 49 MB), and `rusty_quack`, the
community's v2.0.1 build (DuckDB's Rust template demo: two table functions, a scalar, an aggregate), as the official
`duckdb/duckdb:2.0.1` image installed it.

| on main | result |
|---|---|
| `LOAD` directly | refused: built for v2.0.1 only |
| `load_extension(..., repo := 'core')` | refused: not signed by repository 'core' |
| `load_extension(..., repo := 'community')` | loads; a query file (a LIST argument, emoji strings, NULL, a grouped aggregate, 100k rows) gives output identical to v2.0.1 natively |

It found a generator bug: an array of string structs that is NULL (type parameters without names, which
`rusty_quack` passes to `factory_create_type_from_name`) was turned into two uninitialized arrays instead of two NULLs,
and main read garbage. A NULL array now stays NULL.

## Naming and describing loaders

The loader is `v2_capi_shim`, whatever its contract. The contract is baked in per build (`SHIM_CONTRACT_VERSION`), and
so is everything that follows from it: the table, the footer check, the repository path. Policy: the contract is the
latest released DuckDB (v2.0.1 today), bumped with each release; one DuckDB loads one loader, so serves one contract
(a loader carrying several contract tables, chosen by the child's footer, would lift that). Its footer's extension version says what it does, `v2.0.1-on-<target id>`. The loader with capabilities also
sets its description in DuckDB's extension manager (v2 has no call for that; it is what `SetDescription` does for a
C++ extension), and describes each child it loads, with the child's own footer version:

    v2_capi_shim_v201  v2.0.1-on-c826f8f715  C API v2 bridge: runs v2 extensions built for DuckDB v2.0.1 on this
                                             DuckDB (v2.1.0-dev0, c826f8f715), through the v2.0.1 function table;
                                             offers the capabilities connections/1 and interrupts/1. ...
    child2             v0.0.1                v2 extension built for DuckDB v2.0.1, loaded through v2_capi_shim_v201
                                             from .../child2.duckdb_extension

## Installing: install_and_load_extension(name, repo)

The repository name is resolved to its location the way `INSTALL` resolves it: `core` and `community`
(`ExtensionRepository::TryGetKnownRepository`), or a repository created with `CREATE REPOSITORY`
(`ExtensionRepositoryManager::TryGetRepository`); anything else is refused. The loader's contract build is fetched
from `<location>/<contract>/<platform>/<name>.duckdb_extension.gz` with DuckDB's HTTP client and decompressed; its
footer and the repository's signature are checked in memory (`CheckExtensionBufferSignature`, as `INSTALL` checks
before writing); only then is it written, to `<extension directory>/v2_capi_shim/<contract>/<name>.v2_capi_shim`, and
loaded as `load_extension(path, repo)` loads. Not `*.duckdb_extension`: DuckDB lets only `INSTALL` write those
(through a file flag reserved for its installer, which an extension should not use), and the loader's store is not
something DuckDB's own `LOAD` should pick up.

In Docker, on main, with a fresh extension directory and `allow_unsigned_extensions` off:
`SELECT * FROM install_and_load_extension('rusty_quack', 'community')` fetches the community's v2.0.1 build, and the
query file gives output identical to v2.0.1 natively.

## Asking a loader what it is

Both loaders register `v2_capi_shim_info()` (one row: `contract`, `target_version`, `target_source_id`, `platform`,
`kind`, baked in at build time) and `v2_capi_shim_capabilities()` (the named function tables shipped beyond the contract, one row
each: `name`, `version`, `functions`, `description`; none for the plain bridge), so a test can check what it runs on:

    v2.0.1, v2.1.0-dev0, c826f8f715, osx_arm64, bridge+capabilities      (main, this machine)
    v2.0.1, v2.1.0-dev0, 0123456789, linux_arm64, bridge+capabilities    (main built in Docker, no git: placeholder id)
    v2.0.0, v2.0.1, v2.0.1, linux_arm64, bridge                          (the v2.0.0 bridge, official 2.0.1 image)

## A second pair, with community extensions: v2.0.0 -> v2.0.1 (in Docker)

The generator is not tied to one pair: `CONTRACT_DIR=<v2.0.0 tree> TARGET_DIR=<v2.0.1 tree> tools/capi-compat/run.sh`.
Only types that differ between the two are renamed, the log sink adapter is used only when that callback changed,
and parameters the target added take a value from a per-pair file (`pairs/v2.0.0_v2.0.1.json`): v2.0.0 -> v2.0.1 is
619 forwards and 1 generated adapter (`parse_sql` gained `apply_invalidation_policy`; `false` keeps v2.0.0's "parses
and nothing more"). v2.0.1 -> main regenerates as before. Build with `-DCOMPAT_TRACE` to see every adapter call that
returns an error.

Community extensions, surveyed from `duckdb/community-extensions` metadata: of 122 C/Rust ones, 36 build with the
unstable C API; 33 of those are published for v2.0.1 on osx_arm64. Their footers (`fakeit`, `units`, `rusty_quack`
read; nothing executed outside containers) show three groups: real v2.0.1 v2 builds (`rusty_quack`), v2 builds for
v2.0.0 published under the v2.0.1 path (`fakeit`, `units`: DuckDB 2.0.1 refuses to `INSTALL` them, "built
specifically for DuckDB version 'v2.0.0'"), and builds with only the old v1 unstable entrypoint (`lsh`,
`quack_flamegraph`, `read_stat`), which DuckDB 2's loader cannot load at all.

`tools/capi-compat/runtime/docker_v200.sh`, everything in containers: the official `duckdb/duckdb:2.0.0` image
installs `fakeit` and `units` from community (signature checked) and runs a query file; the official
`duckdb/duckdb:2.0.1` image refuses those exact files, and runs the same query file with them loaded through the
v2.0.0 -> v2.0.1 bridge (245 KB, built in a Debian container). Output identical for both: `units` (nested STRUCT
results, checked-error structs, NULLs, its table function, 100k rows aggregated) and `fakeit` (types, ranges, 100k
rows). (The copies under the v2.0.1 path are different builds from the v2.0.0-path ones, so the signed v2.0.0-path
copies are the ones used.)

Two fixes the community extensions forced, both now in the loader: its `get_api` answers any name, as DuckDB's does
for a pinned extension (`units` asks for `'2.0.0'`, without the `v`, and got NULL before, then silently registered
nothing), and a child that never fetches its table is refused, as DuckDB's invoker refuses it.

## Deployment model

The bridge is source, generated for one DuckDB version from that version's spec and the contract's: check it in next
to that DuckDB and build it there, loadable or linked statically, like any extension. From then on that DuckDB loads
v2 extensions built against the contract (v2.0.1), unchanged and without per-version builds of them. Extras on top of
the contract: see below.

## Extras: smuggling functions past the contract (`tools/capi-compat/extras/`)

Viable. The loader is built per DuckDB version anyway, so it can implement anything that version can do and hand
it to its children as a table of plain C function pointers, `duckdb_shim_extra/1` (`shim_extra.h`): here
`context_query` (G1, SQL on a connection of its own to the context's database) and `context_interrupted` (G2).
`shim_extra.cpp` implements them in C++ against the target's own headers, and the loader links the target's
`libduckdb_static.a` (and its third_party archives) as a C++ extension does: it carries its own copy of that
version's code, runs it on the host's objects (sound: the loader is built for exactly that version), and exports only
its entrypoint. 51 MB with `-dead_strip`, nearly all of it the engine `Connection` pulls in; the bridge without
capabilities stays at 156 KB. (Resolving against the host's exported C++ symbols instead, `-undefined
dynamic_lookup`, also worked, at 178 KB, but depends on the host exporting them: the CLIs export about 30k.)

Capabilities are named and offered one by one (`connections/1`: `context_query`; `interrupts/1`:
`context_interrupted`). A child receives them through an optional export, `<name>_duckdb_shim_extra(capability,
table)`, which a loader calls before the entrypoint and DuckDB never calls. Not through `get_api`: for a version-pinned
v2 extension DuckDB's `get_api` ignores the name it is given and returns the whole v2 table (a first attempt that asked
it for extras got that table and crashed), and for a stable one an unknown name fails the load. A child that requires
a capability refuses the load in its entrypoint when it was not offered, so the outcome is decided at LOAD.

`child2` is v2.0.1 + capi('connections/1') required, + capi('interrupts/1') optional:

| | result |
|---|---|
| main, through the loader, at LOAD | `child2_sql('CREATE TABLE made_by_child ...')` from inside its exec callback; the user's connection then reads `sum(i) = 3` from that table |
| main, `SELECT * FROM load_extension('child2.duckdb_extension')` after the loader's LOAD | loads; `child2_sql('SELECT 7*6')` -> `42` |
| main, `SET max_execution_time = 500; SELECT * FROM child2_wait('20000')` | `interrupted after 460 ms` |
| v2.0.1, natively | refused at LOAD: `child2 requires v2.0.1 + capability 'connections/1', which this DuckDB does not offer` |
| v2.0.1, natively, the strict `child` (no capabilities) | works |

Loading later works because the loader calls DuckDB's own `InvokeCAPIV2Entrypoint`, which makes a proper extension
handle, with a trampoline as the entrypoint: it fetches DuckDB's table on the child's behalf (DuckDB checks that the
extension did), offers the capabilities, then calls the child with the contract input. `context_interrupted` must use
`InterruptCheck()`: `IsInterrupted()` only sees the Ctrl-C flag, not `max_execution_time`.

DuckDB's own v2 demo extension, `cpp_api_demo` (written with DuckDB's C++ wrapper of the v2 API, `tools/cpp`), as
DuckDB's build produced it at v2.0.1, run on main through the loader against main's own copy of
`test/extension/load_extension_capi_v2.test` (`tools/capi-compat/extras/run_duckdb_test.sh`): all 36 assertions pass,
including its log entries, macro metadata, and `cpp_api_demo_fail`'s load error with its type intact. Against
v2.0.1's copy of the test, natively: 34 pass; the two copies differ only in a parser message main changed. The run
found one gap, fixed: a child must be recorded as loaded (`ExtensionManager::BeginLoad` / `FinishLoad`), or
`duckdb_extensions()` does not list it.

So: `v2.0.1` strict works anywhere a bridge exists, and natively on v2.0.1; `v2.0.1 + capi('connections')` works
where a loader offers that capability and is refused at LOAD everywhere else. Caveat: an extra is C++
against one version's internals, so each one is a small hand-kept piece per loader version, unlike the generated
bridge; that is the price of anything the v2 API does not yet have.

## Not done

- A child that exercises more than registration: scalar/table function callbacks, values, the hand-written log sink.
- `get_api` itself: the compat loader supplies the contract-layout `duckdb_v2_extension_input` and its `get_api`
  (whose signature changed: it gained a length).
- The opposite direction (a newer contract on an older target) and more than one hop.
