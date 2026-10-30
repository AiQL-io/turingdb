#pragma once

#include <stdint.h>
#include <vector>

#include "Path.h"
#include "DumpResult.h"

namespace db {

/// Reads the `metadata` file of a commit directory, which @ref CommitMetaDataDumper writes
class CommitMetaDataLoader {
public:
    /// @param numCommitDataParts the data parts the commit added, the tail of @param dataPartIDs
    /// @param dataPartIDs every data part of the commit, in the order of its history
    [[nodiscard]] static DumpResult<void> loadDataPartIDs(const fs::Path& metadataFile,
                                                          size_t& numCommitDataParts,
                                                          std::vector<uint64_t>& dataPartIDs);
};

}
