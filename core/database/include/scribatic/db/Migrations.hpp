// =============================================================================
//  Migrations.hpp — Forward-only migration ladder keyed on PRAGMA user_version.
//  Index N holds the statements that move the schema from version N to N+1.
// =============================================================================
#pragma once

#include "scribatic/db/Schema.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scribatic::db {

struct Migration {
    std::int32_t     fromVersion;
    std::string_view statements;
};

/// Bootstrap creates version 1; every later version is reached through this
/// ladder, on a fresh install as much as on an upgrade. New entries are
/// appended here and `kSchemaVersion` is incremented in the same commit.
///
/// A migration is several statements. `statements` holds them concatenated,
/// and is executed with sqlite3_exec, which runs each in turn.
inline const std::vector<Migration>& migrations() {
    static const std::string kToV2 = [] {
        std::string sql;
        for (const auto part : {kCreateSpeakers, kCreateWords, kAddSegmentSpeaker,
                                kAddNoteSpeakerCount, kCreateFtsTriggers}) {
            sql.append(part);
            sql.push_back('\n');
        }
        return sql;
    }();
    static const std::string kToV3 = std::string(kAddNoteLayout) + "\n" + std::string(kAddNoteRefined) + "\n";
    static const std::vector<Migration> kMigrations{
        {1, kToV2},
        {2, kToV3},
    };
    return kMigrations;
}

} // namespace scribatic::db
