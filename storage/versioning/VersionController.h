#pragma once

#include <unordered_map>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include <shared_mutex>
#include <atomic>

#include "ID.h"
#include "Path.h"
#include "Profiler.h"
#include "indexes/IndexManager.h"
#include "metadata/SupportedType.h"
#include "versioning/Change.h"
#include "versioning/SubmitMode.h"
#include "versioning/CommitResult.h"
#include "versioning/SquashHistoryResult.h"
#include "mergers/DataPartMergeResult.h"
#include "dump/DumpResult.h"
#include "versioning/Commit.h"
#include "versioning/CommitHash.h"
#include "datapart/DataPart.h"
#include "ArcManager.h"

namespace db {

class Graph;
class GraphLoader;
class GraphDumper;
class JobSystem;
class FrozenCommitTx;

struct EntityIDPair {
    NodeID _nodeID;
    EdgeID _edgeID;
};

class VersionController {
public:
    using CommitMap = std::unordered_map<CommitHash, size_t>;
    using DataPartMap = std::unordered_map<DataPartID, WeakArc<DataPart>>;
    explicit VersionController(Graph* graph);
    ~VersionController();

    VersionController(const VersionController&) = delete;
    VersionController(VersionController&&) = delete;
    VersionController& operator=(const VersionController&) = delete;
    VersionController& operator=(VersionController&&) = delete;

    void createFirstCommit();
    [[nodiscard]] DataPartMergeResult<void> mergeDataParts(JobSystem& jobSystem);
    [[nodiscard]] std::unique_ptr<Change> newChange(CommitHash base = CommitHash::head());

    /// Collects the hashes of the commits after @param base (from the very first commit when
    /// it is empty) up to and including @param tip, oldest first. The commit @param base
    /// stands for comes before them, since squashing needs it loaded as well.
    [[nodiscard]] SquashHistoryResult<void> getSquashRange(std::optional<CommitHash> base,
                                                           CommitHash tip,
                                                           std::vector<CommitHash>& range) const;

    /// Replaces the commits after @param base up to and including @param tip with @param tip,
    /// which keeps its hash and holds the data parts and writes of the whole range.
    /// @param openChangeBases are the base commits of the open changes of the graph.
    /// @param squashed receives the hashes of the commits that are gone.
    [[nodiscard]] SquashHistoryResult<void> squashHistory(std::optional<CommitHash> base,
                                                          CommitHash tip,
                                                          std::span<const CommitHash> openChangeBases,
                                                          std::vector<CommitHash>& squashed);

    [[nodiscard]] FrozenCommitTx openTransaction(CommitHash hash = CommitHash::head()) const;
    [[nodiscard]] CommitHash getHeadHash() const;
    [[nodiscard]] const Graph* getGraph() const { return _graph; }
    [[nodiscard]] const Commit* getCommitSafe(CommitHash hash) const;
    [[nodiscard]] const Commit* getCommitUnsafe(CommitHash hash) const;
    [[nodiscard]] DumpResult<void> loadCommit(CommitHash hash,
                                              const fs::Path& commitDir,
                                              const fs::Path& partsDir,
                                              Graph* graph);

    size_t getNumCommits() const { return _commits.size(); }

    WeakArc<CommitData> createCommitData(CommitHash hash) {
        Profile profile("VersionController::createCommitData");
        return _dataManager->create(hash);
    }

    WeakArc<DataPart> createDataPart(NodeID firstNodeID,
                                     EdgeID firstEdgeID,
                                     DataPartID id = DataPartID::create()) {
        Profile profile("VersionController::createDataPart");
        return _partManager->create(firstNodeID, firstEdgeID, id);
    }

    DataPartMap& getPartMap() { return _partMap; }

    template <SupportedType P>
    [[nodiscard]] WeakArc<Index> createNodePropertyIndex(std::string_view indexName,
                                                         PropertyTypeID ptID);

    template <SupportedType P>
    [[nodiscard]] WeakArc<Index> createEdgePropertyIndex(std::string_view indexName,
                                                         PropertyTypeID ptID);

private:
    friend GraphLoader;
    friend CommitLoader;
    friend GraphDumper;
    friend Change;
    friend Graph;

    Graph* _graph {nullptr};

    std::atomic<Commit*> _head {nullptr};
    std::atomic<uint64_t> _nextChangeID {0};

    mutable std::shared_mutex _mutex;
    Commit::CommitVector _commits;
    CommitMap _offsets;

    /// Commits a history squash removed from @ref _commits. A reader that looked a commit up
    /// before the squash may still hold its pointer, so they live as long as the controller.
    Commit::CommitVector _retiredCommits;
    std::unique_ptr<ArcManager<CommitData>> _dataManager;
    std::unique_ptr<ArcManager<DataPart>> _partManager;
    IndexManager _indexManager;

    DataPartMap _partMap;

    std::unique_lock<std::shared_mutex> lock();

    /// Gets the offset for a given commit hash, or nullopt if not found
    std::optional<size_t> getCommitIndex(CommitHash hash) const;

    void addCommit(std::unique_ptr<Commit> commit);

    [[nodiscard]] CommitResult<void> submitChange(Change* change, JobSystem&, SubmitMode mode);

    [[nodiscard]] Commit::CommitSpan getCommitsSinceCommitHash(CommitHash from) const;

    [[nodiscard]] SquashHistoryResult<void> resolveSquashRange(std::optional<CommitHash> base,
                                                               CommitHash tip,
                                                               size_t& firstIndex,
                                                               size_t& tipIndex) const;

    [[nodiscard]] static std::unique_ptr<Commit> squashCommits(std::vector<std::unique_ptr<Commit>>& commits,
                                                               const Commit* mainHead);
};
}
