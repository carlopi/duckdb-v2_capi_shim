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

## Layout

| | |
|---|---|
| `CMakeLists.txt`, `extension_config.cmake` | the extension build |
| `src/` | the extension: loading, checking, installing, capabilities |
| `generate.py`, `gen_shim.py`, `parse_header.py`, `compat_manual.c` | the generator, Python only, and the hand-written adapters |
| `contracts/<version>/` | the contract: that release's `duckdb_v2.h`, `duckdb_extension_v2.h`, `api_spec/v2` (MIT, DuckDB) |
| `pairs/` | values for parameters a target added, per contract and target |
| `scripts/append_extension_metadata.py` | footer writer, from duckdb/extension-ci-tools (MIT) |
| `docs/design.md` | what the spike found: the diffs, the rules, the tests |
| `spike/` | the spike's hand-built bridges and Docker runs |

The contract is the latest DuckDB release, bumped with each one (`-DV2_CAPI_SHIM_CONTRACT=v2.0.0` to build another).
