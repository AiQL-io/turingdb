#include "ChangeManager.h"

#include <mutex>
#include <shared_mutex>
#include "Graph.h"
#include "JobSystem.h"
#include "versioning/CommitBuilder.h"

using namespace db;

ChangeManager::ChangeManager() = default;

ChangeManager::~ChangeManager() = default;

Change* ChangeManager::createChange(Graph* graph, CommitHash baseHash) {
    std::unique_lock guard(_changesLock);

    auto change = graph->newChange(baseHash);
    const auto id = change->id();
    auto* ptr = change.get();
    _changes.emplace(GraphChangePair {graph, id}, std::move(change));

    return ptr;
}

DataPartMergeResult<void> ChangeManager::mergeDataParts(Graph* graph, JobSystem& jobSystem) {
    std::unique_lock guard(_changesLock);

    if (!isEmpty()) {
        return DataPartMergeError::result(DataPartMergeErrorType::CHANGES_ON_MAIN);
    }

    return graph->mergeDataParts(jobSystem);
}

SquashHistoryResult<void> ChangeManager::squashHistory(Graph* graph,
                                                       std::optional<CommitHash> base,
                                                       CommitHash tip,
                                                       std::vector<CommitHash>& squashed) {
    std::unique_lock guard(_changesLock);

    std::vector<CommitHash> openChangeBases;
    for (const auto& [pair, change] : _changes) {
        if (pair._graph == graph) {
            openChangeBases.emplace_back(change->baseHash());
        }
    }

    return graph->squashHistory(base, tip, openChangeBases, squashed);
}

ChangeResult<Change*> ChangeManager::getChange(const Graph* graph, ChangeID changeID) {
    std::shared_lock guard(_changesLock);

    const auto it = _changes.find(GraphChangePair {graph, changeID});
    if (it == _changes.end()) {
        return ChangeError::result(ChangeErrorType::CHANGE_NOT_FOUND, changeID);
    }

    return it->second.get();
}

ChangeResult<void> ChangeManager::submitChange(ChangeAccessor& access, JobSystem& jobsystem, SubmitMode mode) {
    std::unique_lock guard(_changesLock);
    const Graph* graph = access.getGraph();

    const auto findIt = _changes.find(GraphChangePair {graph, access.getID()});
    if (findIt == _changes.end()) {
        return ChangeError::result(ChangeErrorType::CHANGE_NOT_FOUND, access.getID());
    }

    auto& change = findIt->second;

    if (mode == SubmitMode::SQUASH) {
        for (const auto& [pair, other] : _changes) {
            const bool isOtherChange = other.get() != change.get();
            const bool sameGraph = pair._graph == graph;

            if (isOtherChange && sameGraph && change->hasCommit(other->baseHash())) {
                return ChangeError::result(ChangeErrorType::COULD_NOT_ACCEPT_CHANGE,
                                           CommitError(CommitErrorType::SQUASH_BLOCKED_BY_DEPENDENT_CHANGE));
            }
        }
    }

    if (auto res = change->submit(jobsystem, mode); !res) {
        return ChangeError::result(ChangeErrorType::COULD_NOT_ACCEPT_CHANGE, res.error());
    }

    access.release();
    _changes.erase(findIt);

    return {};
}

ChangeResult<void> ChangeManager::deleteChange(ChangeAccessor& access, ChangeID changeID) {
    std::unique_lock guard(_changesLock);
    const Graph* graph = access.getGraph();

    const auto it = _changes.find(GraphChangePair {graph, changeID});
    if (it == _changes.end()) {
        return ChangeError::result(ChangeErrorType::CHANGE_NOT_FOUND, changeID);
    }

    access.release();
    _changes.erase(it);

    return {};
}

void ChangeManager::listChanges(std::vector<const Change*>& list, const Graph* graph) const {
    std::shared_lock guard(_changesLock);

    list.clear();
    for (const auto& [pair, change] : _changes) {
        if (graph == pair._graph) {
            list.emplace_back(change.get());
        }
    }
}

bool ChangeManager::hasChanges() const {
    std::shared_lock guard(_changesLock);

    return !_changes.empty();
}
