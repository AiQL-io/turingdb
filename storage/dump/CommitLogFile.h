#pragma once

#include <stdint.h>
#include <vector>

#include "Path.h"
#include "DumpResult.h"

namespace db {

/// The `commitlog` file of a graph directory: the hashes of its commits, oldest first. A commit
/// directory is part of the graph only while the log lists it.
class CommitLogFile {
public:
    [[nodiscard]] static DumpResult<void> read(const fs::Path& logFile, std::vector<uint64_t>& hashes);
    [[nodiscard]] static DumpResult<void> write(const fs::Path& logFile, const std::vector<uint64_t>& hashes);
};

}
