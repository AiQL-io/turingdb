#include "GraphCompactor.h"

#include <chrono>
#include <optional>
#include <vector>

#include <spdlog/spdlog.h>

#include "Graph.h"
#include "SystemAccessor.h"
#include "SystemManager.h"
#include "TuringException.h"
#include "versioning/Commit.h"
#include "versioning/CommitData.h"
#include "versioning/VersionController.h"

using namespace db;

GraphCompactor::GraphCompactor(SystemManager* system, Duration retention)
    : _system(system),
    _retention(retention)
{
}

GraphCompactor::~GraphCompactor() {
    stop();
}

void GraphCompactor::start(Duration interval) {
    if (interval.getMicroseconds() <= 0 || _thread.joinable()) {
        return;
    }

    _interval = interval;
    _stopping = false;
    _thread = std::thread(&GraphCompactor::run, this);
}

void GraphCompactor::stop() {
    {
        std::unique_lock lock(_mutex);
        _stopping = true;
    }

    _wakeUp.notify_all();

    if (_thread.joinable()) {
        _thread.join();
    }
}

void GraphCompactor::compactGraphs(DateTime now) {
    SystemAccessor system = _system->accessShared();

    std::vector<std::string_view> graphNames;
    system.listGraphs(graphNames);

    for (const std::string_view graphName : graphNames) {
        if (_retention.getMicroseconds() > 0) {
            squashOldCommits(system, graphName, now);
        }

        mergeDataParts(system, graphName);
    }
}

void GraphCompactor::run() {
    const std::chrono::microseconds interval {_interval.getMicroseconds()};

    std::unique_lock lock(_mutex);
    while (!_wakeUp.wait_for(lock, interval, [this] { return _stopping; })) {
        lock.unlock();

        try {
            compactGraphs(DateTime::now());
        } catch (const TuringException& error) {
            spdlog::error("Graph compaction failed: {}", error.what());
        }

        lock.lock();
    }
}

void GraphCompactor::squashOldCommits(SystemAccessor& system, std::string_view graphName, DateTime now) {
    const Graph* graph = system.getGraph(graphName);
    if (!graph) {
        return;
    }

    const std::optional<CommitHash> tip = graph->getVersionController().getSquashTipBefore(now - _retention);
    if (!tip) {
        return;
    }

    if (const auto result = system.squashCommits(graphName, std::nullopt, *tip); !result) {
        spdlog::info("Did not squash the old commits of graph {}: {}", graphName, result.error().fmtMessage());
        return;
    }

    spdlog::info("Squashed the commits of graph {} through {}", graphName, *tip);
}

void GraphCompactor::mergeDataParts(SystemAccessor& system, std::string_view graphName) {
    Graph* graph = system.getGraph(graphName);
    if (!graph) {
        return;
    }

    const VersionController& versions = graph->getVersionController();
    const Commit* head = versions.getCommitSafe(graph->getHeadHash());
    const size_t dataPartCount = head->data().allDataparts().size();
    if (dataPartCount <= MERGE_DATA_PART_COUNT) {
        return;
    }

    if (const auto result = system.mergeDataParts(graph); !result) {
        spdlog::info("Did not merge the {} data parts of graph {}: {}", dataPartCount, graphName, result.error().fmtMessage());
        return;
    }

    spdlog::info("Merged the {} data parts of graph {}", dataPartCount, graphName);
}
