#include "Graph.h"

#include "views/GraphView.h"
#include "writers/DataPartBuilder.h"
#include "versioning/Change.h"
#include "versioning/Commit.h"
#include "versioning/VersionController.h"
#include "versioning/Transaction.h"

using namespace db;

Graph::Graph()
    : _graphName("default"),
    _graphPath("/dev/null"),
    _versionController(new VersionController {this})
{
}

Graph::Graph(const std::string& name, const fs::Path& path)
    : _graphName(name),
    _graphPath(path),
    _versionController(new VersionController {this})
{
}

Graph::~Graph() {
}

std::unique_ptr<Change> Graph::newChange(CommitHash base) {
    return _versionController->newChange(base);
}

FrozenCommitTx Graph::openTransaction(CommitHash hash) const {
    return _versionController->openTransaction(hash);
}

DataPartMergeResult<void> Graph::mergeDataParts(JobSystem& jobSystem) {
    return _versionController->mergeDataParts(jobSystem);
}

SquashHistoryResult<void> Graph::getSquashRange(std::optional<CommitHash> base,
                                                CommitHash tip,
                                                std::vector<CommitHash>& range) const {
    return _versionController->getSquashRange(base, tip, range);
}

SquashHistoryResult<void> Graph::squashHistory(std::optional<CommitHash> base,
                                               CommitHash tip,
                                               std::span<const CommitHash> openChangeBases,
                                               std::vector<CommitHash>& squashed) {
    return _versionController->squashHistory(base, tip, openChangeBases, squashed);
}

CommitHash Graph::getHeadHash() const {
    return _versionController->getHeadHash();
}

DumpResult<void> Graph::loadCommit(CommitHash hash) {
    const fs::Path commitDir = _graphPath / "commits" / std::to_string(hash.get());
    const fs::Path partsDir = _graphPath / "dataparts";
    return _versionController->loadCommit(hash, commitDir, partsDir, this);
}

std::unique_ptr<Graph> Graph::create() {
    auto* graph = new Graph();
    graph->_versionController->createFirstCommit();
    return std::unique_ptr<Graph>(graph);
}

std::unique_ptr<Graph> Graph::create(const std::string& name, const fs::Path& path) {
    auto* graph = new Graph(name, path);
    graph->_versionController->createFirstCommit();
    return std::unique_ptr<Graph>(graph);
}
