# v2_capi_shim

Runs DuckDB extensions built against the v2 C API of one DuckDB release (the **contract**, v2.0.1) on the DuckDB it is
built with (the **target**), by handing them the contract's function table implemented on the target's. Extensions
are built once, against the contract; each DuckDB builds its own shim.

## Build it into a DuckDB

In that DuckDB's `extension_config.cmake` (or a file passed with `-DDUCKDB_EXTENSION_CONFIGS=...`):

```cmake
duckdb_extension_load(v2_capi_shim SOURCE_DIR /path/to/v2_capi_shim)
```

and build DuckDB as usual. That is all it takes: no change to DuckDB's sources, and nothing to install beyond what
DuckDB's build already needs (`python3`). At configure time `generate.py` reads the target's `src/include/duckdb_v2.h`
and the contract's (`contracts/v2.0.1/`) and generates the adapters; configure fails, naming the functions, if the
target changed something no rule covers. The result is `extension/v2_capi_shim/v2_capi_shim.duckdb_extension`
(loadable), or linked into DuckDB like any other extension.

## Use it

```sql
LOAD v2_capi_shim;
-- a file: its footer must say a v2 build for the contract, and (unless allow_unsigned_extensions) the named
-- repository's key must have signed it
SELECT * FROM load_extension('/path/to/ext.duckdb_extension', repo := 'community');
-- or fetched from a repository (core, community, or one made with CREATE REPOSITORY), for the contract version
SELECT * FROM install_and_load_extension('rusty_quack', 'community');

SELECT * FROM v2_capi_shim_info();          -- contract, target version and source id, platform
SELECT * FROM v2_capi_shim_capabilities();  -- named function tables offered beyond the contract
```

Beyond the contract, the shim offers capabilities a child can take (it exports `<name>_duckdb_shim_extra(capability,
table)`; DuckDB itself never calls it): `connections/1` (SQL from a callback's context) and `interrupts/1` (whether
the query was interrupted). See `src/include/shim_extra.h`.

## Build it on its own

The shim is a C++ extension, built inside a DuckDB source tree: any DuckDB you want it for, checked out as `duckdb/`
(not a submodule: the shim belongs to no one DuckDB, and CI checks out the one it builds for):

```sh
git submodule update --init                                    # extension-ci-tools
git clone --branch v2.0.1 https://github.com/duckdb/duckdb     # or any DuckDB with the v2 C API
GEN=ninja make                                                 # build/release/duckdb, .../v2_capi_shim.duckdb_extension
make test                                                      # test/sql
make test_shim                                                 # tests/run.sh: a v2.0.1 extension loaded through it
```

## CI: build it for a given DuckDB

`.github/workflows/build.yml` builds the shim with extension-ci-tools' distribution workflow (DuckDB's own build
images; Linux and macOS: `linux_amd64`, `linux_arm64`, `osx_amd64`, `osx_arm64`, and musl), runs `test/sql`, and uploads
`v2_capi_shim-<duckdb version>-extension-<platform>`. Which DuckDB:

- by hand: run the workflow with `duckdb_ref` (a tag, branch or commit)
- by callback, e.g. from whatever publishes a DuckDB nightly or release:
  `POST /repos/carlopi/duckdb-v2_capi_shim/dispatches` with
  `{"event_type": "build-for-duckdb", "client_payload": {"duckdb_ref": "<sha or tag>"}}`
  (`duckdb-nightly`, `duckdb-release` work the same; the sender needs a token allowed to dispatch to this repository)
- otherwise (push, pull request, nightly schedule): `main`, where the v2 C API moves; a new release comes in by
  callback (`duckdb-release` with its tag)

Builds on `main` (and by hand or callback) are then published: the `notify` job asks
`carlopi/example-duckdb-extension-repository` to publish the run; that repository checks the run, signs the builds with
its own key, and serves them (GitHub Pages) at `<version>/<platform>/v2_capi_shim.duckdb_extension.gz`. Then:
`CREATE EXTENSION REPOSITORY carlopi.github.io; INSTALL AND LOAD v2_capi_shim FROM carlopi.github.io;`

`tests/run.sh <duckdb> <v2_capi_shim.duckdb_extension>` runs the same test against any build.

## Layout

| | |
|---|---|
| `CMakeLists.txt`, `extension_config.cmake` | the extension build |
| `src/` | the extension: loading, checking, installing, capabilities |
| `generate.py`, `gen_shim.py`, `parse_header.py`, `compat_manual.c` | the generator, Python only, and the hand-written adapters |
| `contracts/<version>/` | the contract: that release's `duckdb_v2.h`, `duckdb_extension_v2.h`, `api_spec/v2` (MIT, DuckDB) |
| `pairs/` | values for parameters a target added, per contract and target |
| `extension-ci-tools/` | submodule: DuckDB's extension build tooling (the `Makefile` includes its makefiles) |
| `Makefile`, `test/sql/` | the extension template's build (`make`, `make test`); `make test_shim` runs `tests/run.sh` |
| `tests/` | `child.c` (a v2.0.1 extension) and `run.sh` |
| `.github/workflows/build.yml` | CI |
| `docs/design.md` | what the spike found: the diffs, the rules, the tests |
| `spike/` | the spike's hand-built bridges and Docker runs |

The contract is the latest DuckDB release, bumped with each one (`-DV2_CAPI_SHIM_CONTRACT=v2.0.0` to build another).
