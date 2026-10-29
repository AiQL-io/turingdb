#pragma once

#include <span>

#include "Path.h"
#include "DumpResult.h"
#include "versioning/CommitHash.h"

namespace db {

class Commit;

/// Writes a squash of the commit history to the graph directory, so that a restart brings back
/// the squashed history and never a half of it.
///
/// The squashed commit is prepared next to the files it replaces, in `<hash>.squash` and
/// `commitlog.squash`. Renaming the prepared commit log into place is the moment the squash
/// happens; a directory left behind by a crash before that is discarded, and a crash after
/// the marker `commitlog.squash` exists is finished by @ref recover.
class HistorySquashDumper {
public:
    /// Replaces the directory of @param tip, which now holds the writes and data parts of the
    /// whole range, and drops @param squashed from the commit log and from disk.
    [[nodiscard]] static DumpResult<void> dump(const Commit& tip,
                                               const fs::Path& graphDir,
                                               std::span<const CommitHash> squashed);

    /// Finishes or discards a squash a crash interrupted. Called before a graph is loaded.
    [[nodiscard]] static DumpResult<void> recover(const fs::Path& graphDir);
};

}
