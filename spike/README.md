The spike as it was developed inside duckdb-container (`tools/capi-compat/`): hand-built bridges and loaders, Docker
runs with community extensions, and the tests that found the rules. The scripts assume that repository's layout
(`duckdb/` submodule, `.cache/duckdb-main`, `extension-ci-tools/`); kept for reference. The build is now CMake, through
DuckDB's own extension build (see the top-level README). `docs/design.md` records what the spike found.
