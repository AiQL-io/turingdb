#pragma once

#include "Path.h"
#include "DumpResult.h"

namespace db {

class Graph;

class GraphDumper {
public:
    [[nodiscard]] static DumpResult<void> dump(const Graph* graph, const fs::Path& graphDir);

    /// Removes the commit directories the commit log does not list and the data part directories
    /// no listed commit uses. Running it again after a crash finishes the job.
    [[nodiscard]] static DumpResult<void> prune(const Graph* graph, const fs::Path& graphDir);

private:
    [[nodiscard]] static DumpResult<void> dumpMissingCommits(const Graph* graph, const fs::Path& graphDir);
};

}
