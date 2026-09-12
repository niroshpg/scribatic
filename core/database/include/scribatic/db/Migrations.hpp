// =============================================================================
//  Migrations.hpp — Forward-only migration ladder keyed on PRAGMA user_version.
//  Index N holds the statements that move the schema from version N to N+1.
// =============================================================================
#pragma once

#include "scribatic/db/Schema.hpp"

#include <string_view>
#include <vector>

namespace scribatic::db {

struct Migration {
    std::int32_t     fromVersion;
    std::string_view statements;
};

/// Empty at v1 (bootstrap creates the current schema directly). New entries are
/// appended here and `kSchemaVersion` is incremented in the same commit.
inline const std::vector<Migration>& migrations() {
    static const std::vector<Migration> kMigrations{};
    return kMigrations;
}

} // namespace scribatic::db
