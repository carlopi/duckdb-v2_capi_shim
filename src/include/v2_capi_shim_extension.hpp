#pragma once

#include "duckdb.hpp"

namespace duckdb {

//! Loads extensions built against the v2 C API of one DuckDB release (the contract, baked in at build time: v2.0.1)
//! into the DuckDB this is built with, through a generated bridge from that release's function table to this one's
class V2CapiShimExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
