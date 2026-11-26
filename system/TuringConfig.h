#pragma once

#include "Path.h"
#include "metadata/Duration.h"

namespace db {

class TuringConfig {
public:
    static constexpr int64_t MICROSECONDS_PER_HOUR = 3600ll * 1000 * 1000;
    static constexpr Duration DEFAULT_HISTORY_RETENTION {7 * 24 * MICROSECONDS_PER_HOUR};
    static constexpr Duration DEFAULT_COMPACTION_INTERVAL {MICROSECONDS_PER_HOUR};

    explicit TuringConfig();
    ~TuringConfig();

    TuringConfig(const TuringConfig&);
    TuringConfig(TuringConfig&&) noexcept;
    TuringConfig& operator=(const TuringConfig&);
    TuringConfig& operator=(TuringConfig&&) noexcept;

    const fs::Path& getTuringDir() const { return _turingDir; }
    const fs::Path& getGraphsDir() const { return _graphsDir; }
    const fs::Path& getDataDir() const { return _dataDir; }
    const fs::Path& getVectorDir() const { return _vectorDir; }
    const fs::Path& getUserExtensionsDir() const { return _userExtensionsDir; }
    const fs::Path& getInstallExtensionsDir() const { return _installExtensionsDir; }
    const fs::Path& getLogsDir() const { return _logsDir; }
    const fs::Path& getLockFilePath() const { return _lockFilePath; }
    const fs::Path& getSocketPath() const { return _socketPath; }

    bool isSyncedOnDisk() const { return _syncedOnDisk; }
    bool usingSystemEvents() const { return _systemEvents; }
    bool mmappingVectorIndexes() const { return _mmapVectorIndexes; }
    Duration getHistoryRetention() const { return _historyRetention; }
    Duration getCompactionInterval() const { return _compactionInterval; }

    void setTuringDirectory(const fs::Path& turingDir);
    void setSyncedOnDisk(bool syncedOnDisk) { _syncedOnDisk = syncedOnDisk; }
    void setMmapVectorIndexes(bool mmapVectorIndexes) { _mmapVectorIndexes = mmapVectorIndexes; }
    void useSystemEvents(bool systemEvents) { _systemEvents = systemEvents; }
    void setHistoryRetention(Duration retention) { _historyRetention = retention; }
    void setCompactionInterval(Duration interval) { _compactionInterval = interval; }

    const std::function<void()>& getOnStopRequest() const { return _onStopRequest; }
    void setOnStopRequest(const std::function<void()>& onStopRequest) {
        _onStopRequest = onStopRequest;
    }

private:
    fs::Path _turingDir;
    fs::Path _graphsDir;
    fs::Path _dataDir;
    fs::Path _vectorDir;
    fs::Path _userExtensionsDir;
    fs::Path _installExtensionsDir;
    fs::Path _logsDir;
    fs::Path _lockFilePath;
    fs::Path _socketPath;

    std::function<void()> _onStopRequest;

    bool _syncedOnDisk {true};
    bool _systemEvents {true};
    bool _mmapVectorIndexes {true};
    Duration _historyRetention {DEFAULT_HISTORY_RETENTION};
    Duration _compactionInterval {DEFAULT_COMPACTION_INTERVAL};
};

}
