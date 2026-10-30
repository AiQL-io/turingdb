#include "VersionController.h"

#include <optional>

#include <range/v3/view/enumerate.hpp>
#include <sys/types.h>
#include <shared_mutex>

#include "JobSystem.h"
#include "Graph.h"
#include "dump/CommitLoader.h"
#include "dump/DumpResult.h"
#include "CommitView.h"
#include "mergers/DataPartMerger.h"
#include "writers/DataPartBuilder.h"
#include "reader/GraphReader.h"
#include "versioning/Change.h"
#include "versioning/CommitBuilder.h"
#include "versioning/CommitHash.h"
#include "versioning/CommitHistoryBuilder.h"
#include "versioning/DataPartRebaser.h"
#include "versioning/Transaction.h"
#include "CommitJournal.h"

#include "Profiler.h"
#include "BioAssert.h"

using namespace db;

namespace {

/*
 * A commit that merged data parts is recognised by the data parts it holds: a commit adds its
 * own on top of those of the commit before, while a merge commit holds the merged ones alone,
 * and that is all of its data parts.
 */
bool replacesEarlierDataParts(const Commit& commit, const Commit* previous) {
    if (commit.isMergeCommit()) {
        return true;
    } else if (!previous) {
        return false;
    }

    const size_t allDataParts = commit.data().allDataparts().size();
    const size_t ownDataParts = commit.data().commitDataparts().size();
    const size_t earlierDataParts = previous->data().allDataparts().size();

    return allDataParts == ownDataParts && earlierDataParts > 0;
}

}

VersionController::VersionController(Graph* graph)
    : _graph(graph),
    _dataManager(std::make_unique<ArcManager<CommitData>>()),
    _partManager(std::make_unique<ArcManager<DataPart>>())
{
}

VersionController::~VersionController() {
    _commits.clear();
    _dataManager.reset();
    _partMap.clear(); // Delete all WeakArc<DataPart> before deleting @ref _partManager
    _partManager.reset();
    // @ref ~IndexManager() calls reset() on its arc manager
}

void VersionController::createFirstCommit() {
    auto commitData = _dataManager->create(CommitHash::create());
    // initialise the previous Commit* of the first commit to nullptr
    auto commit = std::make_unique<Commit>(this, commitData, nullptr);

    this->addCommit(std::move(commit));
}

DataPartMergeResult<void> VersionController::mergeDataParts(JobSystem& jobSystem) {
    Profile profile("VersionController::mergeDataParts");

    std::unique_lock<std::shared_mutex> uniqueLock(_mutex);

    Commit* headCommit = _head.load();

    auto newTip = CommitBuilder::prepareMerge(*this,
                                              nullptr,
                                              headCommit);

    const auto merger = DataPartMerger(&headCommit->data(), newTip->metadata());

    newTip->appendBuilder(merger.merge(headCommit->data().allDataparts()));

    auto buildRes = newTip->build(jobSystem);
    if (!buildRes) {
        return DataPartMergeError::result(DataPartMergeErrorType::MERGE_GRAPH_FAILED);
    }

    addCommit(std::move(buildRes.value()));

    return {};
}

FrozenCommitTx VersionController::openTransaction(CommitHash hash) const {
    if (hash == CommitHash::head()) {
        return _head.load()->openTransaction();
    }

    std::shared_lock<std::shared_mutex> sharedLock(_mutex);

    auto it = _offsets.find(hash);
    if (it == _offsets.end()) {
        return FrozenCommitTx(); // Invalid hash
    }

    return _commits[it->second]->openTransaction();
}

CommitHash VersionController::getHeadHash() const {
    std::shared_lock<std::shared_mutex> sharedLock(_mutex);

    const Commit* head = _head.load();
    if (!head) {
        return CommitHash::head();
    }

    return head->hash();
}

CommitResult<void> VersionController::submitChange(Change* change, JobSystem& jobSystem, SubmitMode mode) {
    Profile profile("VersionController::submitChange");

    std::unique_lock<std::shared_mutex> uniqueLock(_mutex);

    // atomic load main
    Commit* headCommit = _head.load();

    // rebase if main has changed under us
    if (headCommit->hash() != change->baseHash()) {
        if (auto res = change->rebase(jobSystem); !res) {
            return res;
        }
    }

    std::vector<std::unique_ptr<Commit>> builtCommits;

    for (auto& commitBuilder : change->_commits) {
        // If this Change has modifications which were not applied by a "COMMIT" command,
        // then flush them now
        if (!commitBuilder->writeBuffer().isFlushed()) {
            commitBuilder->flushWriteBuffer(jobSystem);
        }

        auto buildRes = commitBuilder->build(jobSystem);
        if (!buildRes) {
            return buildRes.get_unexpected();
        }

        auto& newCommit = buildRes.value();

        if (mode == SubmitMode::MERGE) {
            addCommit(std::move(newCommit));
        } else {
            builtCommits.emplace_back(std::move(newCommit));
        }
    }

    // Nothing has been published to main until every commit of the change is built,
    // so a failure above leaves main untouched
    if (mode == SubmitMode::SQUASH) {
        addCommit(squashCommits(builtCommits, headCommit));
    }

    return {};
}

/*
 * The squashed commit is the tip of the change: it already holds the cumulative metadata,
 * tombstones, indexes and data parts of the whole change, so entity IDs are untouched.
 * It only needs to be re-parented onto main and to report the writes and data parts of
 * every commit it replaces.
 */
std::unique_ptr<Commit> VersionController::squashCommits(std::vector<std::unique_ptr<Commit>>& commits,
                                                         const Commit* mainHead) {
    bioassert(!commits.empty(), "a change always has at least one commit");

    std::unique_ptr<Commit> tip = std::move(commits.back());
    commits.pop_back();

    CommitHistory& tipHistory = tip->history();
    CommitHistoryBuilder historyBuilder {tipHistory};

    size_t numNodes = tip->getNumNodes();
    size_t numEdges = tip->getNumEdges();
    size_t numDataParts = tip->getNumDataParts();

    for (const auto& commit : commits) {
        historyBuilder.absorbJournal(commit->history());

        numNodes += commit->getNumNodes();
        numEdges += commit->getNumEdges();
        numDataParts += commit->getNumDataParts();
    }

    // The data parts of the change are the tail of the cumulative list of the tip
    const size_t mainDataPartCount = mainHead->data().allDataparts().size();
    const size_t totalDataPartCount = tipHistory.allDataparts().size();
    bioassert(totalDataPartCount - mainDataPartCount == numDataParts,
              "squashed data part count mismatch");

    historyBuilder.setCommitDatapartCount(numDataParts);

    tip->setCounts(numNodes, numEdges, numDataParts);
    tip->setPreviousCommit(mainHead);

    return tip;
}

SquashHistoryResult<void> VersionController::resolveSquashRange(std::optional<CommitHash> base,
                                                                CommitHash tip,
                                                                size_t& firstIndex,
                                                                size_t& tipIndex) const {
    const CommitHash tipHash = tip == CommitHash::head() ? _head.load()->hash() : tip;
    const std::optional<size_t> tipOffset = getCommitIndex(tipHash);

    if (!tipOffset.has_value()) {
        return SquashHistoryError::result(SquashHistoryErrorType::COMMIT_NOT_FOUND);
    }

    firstIndex = 0;
    tipIndex = *tipOffset;

    if (base.has_value()) {
        const CommitHash baseHash = *base == CommitHash::head() ? _head.load()->hash() : *base;
        const std::optional<size_t> baseOffset = getCommitIndex(baseHash);

        if (!baseOffset.has_value()) {
            return SquashHistoryError::result(SquashHistoryErrorType::COMMIT_NOT_FOUND);
        } else if (*baseOffset >= *tipOffset) {
            return SquashHistoryError::result(SquashHistoryErrorType::RANGE_NOT_ORDERED);
        }

        firstIndex = *baseOffset + 1;
    }

    return {};
}

SquashHistoryResult<void> VersionController::getSquashRange(std::optional<CommitHash> base,
                                                            CommitHash tip,
                                                            std::vector<CommitHash>& range) const {
    std::shared_lock<std::shared_mutex> sharedLock(_mutex);

    size_t firstIndex = 0;
    size_t tipIndex = 0;
    if (auto res = resolveSquashRange(base, tip, firstIndex, tipIndex); !res) {
        return res;
    }

    // The commit before the range is part of what has to be loaded: it tells whether the first
    // commit of the range merged data parts
    range.clear();
    if (firstIndex > 0) {
        range.emplace_back(_commits[firstIndex - 1]->hash());
    }

    for (size_t index = firstIndex; index <= tipIndex; index++) {
        range.emplace_back(_commits[index]->hash());
    }

    return {};
}

/*
 * The squashed commit is the last commit of the range: it already holds the cumulative
 * metadata, tombstones, indexes and data parts of the whole range, so entity IDs are
 * untouched. It only needs to be re-parented onto the commit before the range and to report
 * the writes and data parts of every commit it replaces.
 */
SquashHistoryResult<void> VersionController::squashHistory(std::optional<CommitHash> base,
                                                           CommitHash tip,
                                                           std::span<const CommitHash> openChangeBases,
                                                           std::vector<CommitHash>& squashed) {
    Profile profile("VersionController::squashHistory");

    std::unique_lock<std::shared_mutex> uniqueLock(_mutex);

    size_t firstIndex = 0;
    size_t tipIndex = 0;
    if (auto res = resolveSquashRange(base, tip, firstIndex, tipIndex); !res) {
        return res;
    }

    squashed.clear();

    // A single commit has nothing to be folded into
    if (firstIndex == tipIndex) {
        return {};
    }

    const Commit* baseCommit = firstIndex > 0 ? _commits[firstIndex - 1].get() : nullptr;
    if (baseCommit && !baseCommit->hasData()) {
        return SquashHistoryError::result(SquashHistoryErrorType::COMMIT_NOT_LOADED);
    }

    size_t firstAbsorbedIndex = firstIndex;
    bool replacesDataParts = false;

    for (size_t index = firstIndex; index <= tipIndex; index++) {
        const Commit& commit = *_commits[index];
        const Commit* previous = index > 0 ? _commits[index - 1].get() : nullptr;

        if (!commit.hasData()) {
            return SquashHistoryError::result(SquashHistoryErrorType::COMMIT_NOT_LOADED);
        } else if (replacesEarlierDataParts(commit, previous)) {
            firstAbsorbedIndex = index;
            replacesDataParts = true;
        }
    }

    // A change based on the last commit of the range, or on any commit after it, is rebased onto
    // the squashed commit as it was on the original. One based on a commit that goes away has
    // nothing to be rebased from.
    for (const CommitHash changeBase : openChangeBases) {
        const std::optional<size_t> baseOffset = getCommitIndex(changeBase);

        if (baseOffset.has_value() && *baseOffset >= firstIndex && *baseOffset < tipIndex) {
            return SquashHistoryError::result(SquashHistoryErrorType::BLOCKED_BY_OPEN_CHANGE);
        }
    }

    Commit* tipCommit = _commits[tipIndex].get();
    CommitHistory& tipHistory = tipCommit->history();
    CommitHistoryBuilder historyBuilder {tipHistory};

    size_t numNodes = tipCommit->getNumNodes();
    size_t numEdges = tipCommit->getNumEdges();
    size_t numDataParts = tipCommit->data().commitDataparts().size();

    for (size_t index = firstAbsorbedIndex; index < tipIndex; index++) {
        const Commit& commit = *_commits[index];

        historyBuilder.absorbJournal(commit.history());

        numNodes += commit.getNumNodes();
        numEdges += commit.getNumEdges();
        numDataParts += commit.data().commitDataparts().size();
    }

    // The data parts of the range are the tail of the cumulative list of the last commit
    const size_t baseDataPartCount = baseCommit ? baseCommit->data().allDataparts().size() : 0;
    const size_t totalDataPartCount = tipHistory.allDataparts().size();

    if (replacesDataParts) {
        numDataParts = totalDataPartCount;
        tipCommit->markMergeCommit();
    } else {
        bioassert(totalDataPartCount - baseDataPartCount == numDataParts,
                  "squashed data part count mismatch");
    }

    historyBuilder.setCommitDatapartCount(numDataParts);

    tipCommit->setCounts(numNodes, numEdges, numDataParts);
    tipCommit->setPreviousCommit(baseCommit);

    for (size_t index = firstIndex; index < tipIndex; index++) {
        squashed.emplace_back(_commits[index]->hash());
        _offsets.erase(_commits[index]->hash());
        _retiredCommits.emplace_back(std::move(_commits[index]));
    }

    _commits.erase(_commits.begin() + firstIndex, _commits.begin() + tipIndex);

    for (size_t index = firstIndex; index < _commits.size(); index++) {
        _offsets[_commits[index]->hash()] = index;
    }

    return {};
}

std::unique_ptr<Change> VersionController::newChange(CommitHash base) {
    return Change::create(this, ChangeID {_nextChangeID.fetch_add(1)}, base);
}

std::unique_lock<std::shared_mutex> VersionController::lock() {
    return std::unique_lock<std::shared_mutex> {_mutex};
}

// Needs to be called in a locked context
void VersionController::addCommit(std::unique_ptr<Commit> commit) {
    auto* ptr = commit.get();

    _offsets.emplace(commit->hash(), _commits.size());
    _commits.emplace_back(std::move(commit));
    _head.store(ptr);
}

std::optional<size_t> VersionController::getCommitIndex(CommitHash hash) const {
    auto it = _offsets.find(hash);

    if (it == _offsets.end()) {
        return std::nullopt;
    }

    return it->second;
}

const Commit* VersionController::getCommitUnsafe(CommitHash hash) const {
    if (hash == CommitHash::head()) {
        return _head.load();
    }

    const std::optional<size_t> offset = getCommitIndex(hash);

    if (!offset.has_value()) {
        return nullptr;
    }

    return _commits[*offset].get();
}

const Commit* VersionController::getCommitSafe(CommitHash hash) const {
    std::shared_lock lock(_mutex);

    if (hash == CommitHash::head()) {
        return _head.load();
    }

    const std::optional<size_t> offset = getCommitIndex(hash);

    if (!offset.has_value()) {
        return nullptr;
    }

    return _commits[*offset].get();
}

// NOTE: Called within locked-context
Commit::CommitSpan VersionController::getCommitsSinceCommitHash(CommitHash from) const {
    // Should not be trying to check conflicts if we are not rebasing
    bioassert(from != CommitHash::head(), "should not be checking conflicts if not rebasing");

    const std::optional<size_t> startIndexOpt = getCommitIndex(from);
    bioassert(startIndexOpt, "Could not find Commit with hash {:x}", from.get());

    const size_t startIndex = *startIndexOpt;

    // +1 to skip the commit we branched from
    const auto* spanStart = _commits.data() + startIndex + 1;
    const size_t numCommitsSinceFrom = _commits.size() - (startIndex + 1);

    return {spanStart, numCommitsSinceFrom};
}

DumpResult<void> VersionController::loadCommit(CommitHash hash,
                                               const fs::Path& commitDir,
                                               const fs::Path& partsDir,
                                               Graph* graph) {
    std::shared_lock lock(_mutex);

    const auto offset = getCommitIndex(hash);
    if (!offset.has_value()) {
        return DumpError::result(DumpErrorType::COMMIT_HASH_NOT_FOUND);
    }

    Commit* commit = _commits[offset.value()].get();
    if (commit->hasData()) {
        return {}; // already loaded — success
    }

    auto res = CommitLoader::loadData(commitDir, partsDir, graph, commit);
    if (!res) {
        return res.get_unexpected();
    }
    return {};
}


template <SupportedType P>
WeakArc<Index> VersionController::createNodePropertyIndex(std::string_view indexName,
                                                          PropertyTypeID ptID) {
    return _indexManager.createNodeIndex<P>(indexName, ptID);
}

template <SupportedType P>
WeakArc<Index> VersionController::createEdgePropertyIndex(std::string_view indexName,
                                                          PropertyTypeID ptID) {
    return _indexManager.createEdgeIndex<P>(indexName, ptID);
}

namespace db {
template WeakArc<Index> VersionController::createNodePropertyIndex<types::Int64>(std::string_view indexName, PropertyTypeID ptID);
template WeakArc<Index> VersionController::createNodePropertyIndex<types::UInt64>(std::string_view indexName, PropertyTypeID ptID);
template WeakArc<Index> VersionController::createNodePropertyIndex<types::Double>(std::string_view indexName, PropertyTypeID ptID);
template WeakArc<Index> VersionController::createNodePropertyIndex<types::String>(std::string_view indexName, PropertyTypeID ptID);
template WeakArc<Index> VersionController::createNodePropertyIndex<types::Bool>(std::string_view indexName, PropertyTypeID ptID);
template WeakArc<Index> VersionController::createNodePropertyIndex<types::Embedding>(std::string_view indexName, PropertyTypeID ptID);
template WeakArc<Index> VersionController::createNodePropertyIndex<types::List>(std::string_view indexName, PropertyTypeID ptID);
template WeakArc<Index> VersionController::createNodePropertyIndex<types::DateTime>(std::string_view indexName, PropertyTypeID ptID);
template WeakArc<Index> VersionController::createNodePropertyIndex<types::Duration>(std::string_view indexName, PropertyTypeID ptID);
template WeakArc<Index> VersionController::createNodePropertyIndex<types::Map>(std::string_view indexName, PropertyTypeID ptID);

template WeakArc<Index> VersionController::createEdgePropertyIndex<types::Int64>(std::string_view indexName, PropertyTypeID ptID);
template WeakArc<Index> VersionController::createEdgePropertyIndex<types::UInt64>(std::string_view indexName, PropertyTypeID ptID);
template WeakArc<Index> VersionController::createEdgePropertyIndex<types::Double>(std::string_view indexName, PropertyTypeID ptID);
template WeakArc<Index> VersionController::createEdgePropertyIndex<types::String>(std::string_view indexName, PropertyTypeID ptID);
template WeakArc<Index> VersionController::createEdgePropertyIndex<types::Bool>(std::string_view indexName, PropertyTypeID ptID);
template WeakArc<Index> VersionController::createEdgePropertyIndex<types::Embedding>(std::string_view indexName, PropertyTypeID ptID);
template WeakArc<Index> VersionController::createEdgePropertyIndex<types::List>(std::string_view indexName, PropertyTypeID ptID);
template WeakArc<Index> VersionController::createEdgePropertyIndex<types::DateTime>(std::string_view indexName, PropertyTypeID ptID);
template WeakArc<Index> VersionController::createEdgePropertyIndex<types::Duration>(std::string_view indexName, PropertyTypeID ptID);
template WeakArc<Index> VersionController::createEdgePropertyIndex<types::Map>(std::string_view indexName, PropertyTypeID ptID);
}
