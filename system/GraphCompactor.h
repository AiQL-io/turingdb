#pragma once

#include <stddef.h>

#include <condition_variable>
#include <mutex>
#include <string_view>
#include <thread>

#include "metadata/DateTime.h"
#include "metadata/Duration.h"

namespace db {

class SystemAccessor;
class SystemManager;

class GraphCompactor {
public:
    static constexpr size_t MERGE_DATA_PART_COUNT = 16;

    GraphCompactor(SystemManager* system, Duration retention);
    ~GraphCompactor();

    GraphCompactor(const GraphCompactor&) = delete;
    GraphCompactor& operator=(const GraphCompactor&) = delete;

    void start(Duration interval);
    void stop();

    void compactGraphs(DateTime now);

private:
    SystemManager* _system {nullptr};
    Duration _retention;
    Duration _interval;
    std::thread _thread;
    std::mutex _mutex;
    std::condition_variable _wakeUp;
    bool _stopping {false};

    void run();
    void squashOldCommits(SystemAccessor& system, std::string_view graphName, DateTime now);
    void mergeDataParts(SystemAccessor& system, std::string_view graphName);
};

}
