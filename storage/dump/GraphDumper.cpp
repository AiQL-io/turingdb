#include "GraphDumper.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

#include "CommitLogFile.h"
#include "CommitMetaDataLoader.h"
#include "FileReader.h"
#include "FileWriter.h"
#include "Graph.h"
#include "GraphInfoDumper.h"
#include "CommitDumper.h"
#include "GraphFileType.h"
#include "FileUtils.h"
#include "versioning/Commit.h"
#include "versioning/Transaction.h"
#include "versioning/VersionController.h"
#include "FilePageReader.h"
#include "GraphInfoLoader.h"

using namespace db;

namespace {

using StdPath = std::filesystem::path;

bool parseDirectoryID(const StdPath& directory, uint64_t& id) {
    const std::string name = directory.filename().string();

    if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }

    id = std::stoull(name);

    return true;
}

DumpResult<void> removeUnreferenced(const StdPath& parent, const std::unordered_set<uint64_t>& referenced) {
    std::error_code error;
    std::vector<StdPath> unreferenced;

    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator {parent, error}) {
        uint64_t id = 0;

        // Names that are not an ID, like the `.squash` leftovers, belong to a recovery
        if (parseDirectoryID(entry.path(), id) && !referenced.contains(id)) {
            unreferenced.emplace_back(entry.path());
        }
    }

    if (error) {
        return DumpError::result(DumpErrorType::CANNOT_PRUNE_GRAPH);
    }

    for (const StdPath& directory : unreferenced) {
        std::filesystem::remove_all(directory, error);

        if (error) {
            return DumpError::result(DumpErrorType::CANNOT_PRUNE_GRAPH);
        }
    }

    return {};
}

// A commit the log does not list, and the data parts it brings, are what a dump that failed
// half way left behind
DumpResult<void> removeInterruptedDump(const Commit& commit, const StdPath& graphDirectory) {
    std::error_code error;

    std::filesystem::remove_all(graphDirectory / "commits" / std::to_string(commit.hash().get()), error);
    if (error) {
        return DumpError::result(DumpErrorType::CANNOT_PRUNE_GRAPH);
    }

    for (const auto& part : commit.data().commitDataparts()) {
        std::filesystem::remove_all(graphDirectory / "dataparts" / std::to_string(part->getID().get()), error);
        if (error) {
            return DumpError::result(DumpErrorType::CANNOT_PRUNE_GRAPH);
        }
    }

    return {};
}

}

DumpResult<void> GraphDumper::prune(const Graph* graph, const fs::Path& graphDir) {
    Profile profile("GraphDumper::prune");

    auto lock = graph->_versionController->lock();

    std::vector<uint64_t> commitHashes;
    if (auto res = CommitLogFile::read(graphDir / "commitlog", commitHashes); !res) {
        return res;
    }

    std::unordered_set<uint64_t> referencedCommits;
    std::unordered_set<uint64_t> referencedDataParts;
    std::vector<uint64_t> dataPartIDs;

    for (const uint64_t hash : commitHashes) {
        size_t numCommitDataParts = 0;
        const fs::Path metadataFile = graphDir / "commits" / std::to_string(hash) / "metadata";

        if (auto res = CommitMetaDataLoader::loadDataPartIDs(metadataFile, numCommitDataParts, dataPartIDs); !res) {
            return res;
        }

        referencedCommits.insert(hash);
        referencedDataParts.insert(dataPartIDs.begin(), dataPartIDs.end());
    }

    const StdPath graphDirectory {graphDir.get()};

    if (auto res = removeUnreferenced(graphDirectory / "commits", referencedCommits); !res) {
        return res;
    }

    return removeUnreferenced(graphDirectory / "dataparts", referencedDataParts);
}

DumpResult<void> GraphDumper::dump(const Graph* graph, const fs::Path& graphDir) {
    Profile profile("GraphDumper::dump");

    spdlog::info("Dumping graph {}", graph->getName());

    auto lock = graph->_versionController->lock();

    if (!graphDir.exists()) {
        // Create directory
        if (auto res = graphDir.mkdir(); !res) {
            return DumpError::result(DumpErrorType::CANNOT_MKDIR_GRAPH, res.error());
        }
    }

    // Dumping graph info
    {
        Profile profile("GraphDumper::dump <graph info>");
        const fs::Path infoFile = graphDir / "info";

        if (infoFile.exists()) {
            auto reader = fs::FilePageReader::open(infoFile, DumpConfig::PAGE_SIZE);
            if (!reader) {
                return DumpError::result(DumpErrorType::CANNOT_OPEN_GRAPH_INFO, reader.error());
            }

            const GraphInfoLoader loader(reader.value());

            auto res = loader.isSameGraph(*graph);
            if (!res) {
                return res.get_unexpected();
            }

            if (!res.value()) {
                return DumpError::result(DumpErrorType::GRAPH_DIR_ALREADY_EXISTS);
            }

            // Graph is already dumped, only dump what's missing
            return GraphDumper::dumpMissingCommits(graph, graphDir);
        }

        auto writer = fs::FilePageWriter::open(infoFile, DumpConfig::PAGE_SIZE);
        if (!writer) {
            return DumpError::result(DumpErrorType::CANNOT_OPEN_GRAPH_INFO, writer.error());
        }

        GraphInfoDumper dumper(writer.value());

        if (auto res = dumper.dump(*graph); !res) {
            return res;
        }
    }

    // Dump graph type
    {
        Profile profile("GraphDumper::dump <graph type>");
        const fs::Path graphTypeFile = graphDir / "type";

        if (graphTypeFile.exists()) {
        }

        const auto typeTag = GraphFileTypeDescription::value(GraphFileType::BINARY);
        if (!FileUtils::writeFile(graphTypeFile.get(), std::string(typeTag))) {
            return DumpError::result(DumpErrorType::CANNOT_WRITE_GRAPH_TYPE);
        }
    }

    // Dump Commits and CommitLog
    {
        Profile profile {"GraphDumper::dump <Commit Log>"};
        const fs::Path logFile = graphDir / "commitlog";

        const Commit* commit = graph->_versionController->_head.load();
        std::vector<CommitHash> commitList;

        // collect all the commits so we can iterate and write them to file later
        while (commit) {
            commitList.emplace_back(commit->hash());

            const std::string fileName = fmt::format("{}", commit->hash().get());
            const fs::Path commitDir = graphDir / "commits" / fileName;
            const fs::Path partsDir = graphDir / "dataparts";

            if (commitDir.exists()) {
                // commit shouldn't exist
                return DumpError::result(DumpErrorType::COMMIT_ALREADY_EXISTS);
            }

            if (auto res = CommitDumper::dump(*commit, commitDir, partsDir); !res) {
                return res;
            }

            commit = commit->getPreviousCommit();
        }

        auto fileRes = fs::File::createAndOpen(logFile);
        if (!fileRes) {
            return DumpError::result(DumpErrorType::CANNOT_OPEN_COMMIT_LOG, fileRes.error());
        }

        fs::FileWriter<> writer;
        writer.setFile(&fileRes.value());

        GraphDumpHelper::writeFileHeader(&writer);

        // Dump the number of commits.
        writer.write(graph->_versionController->getNumCommits());

        // Iterate through the list backwards to dump commits in ascending (by time of commit)
        // order
        for (const CommitHash hash : commitList | std::views::reverse) {
            writer.write(hash.get());
        }

        writer.flush();
        if (writer.errorOccured()) {
            return DumpError::result(DumpErrorType::COULD_NOT_WRITE_COMMIT_LOG,
                                     *writer.error());
        }
    }

    return {};
}

DumpResult<void> GraphDumper::dumpMissingCommits(const Graph* graph, const fs::Path& graphDir) {
    const fs::Path logFile = graphDir / "commitlog";
    const fs::Path partsDir = graphDir / "dataparts";

    std::vector<uint64_t> hashes;
    if (auto res = CommitLogFile::read(logFile, hashes); !res) {
        return res;
    }

    if (hashes.empty()) {
        return DumpError::result(DumpErrorType::COMMIT_HASH_NOT_FOUND);
    }

    const uint64_t lastDumpedHash = hashes.back();

    std::vector<const Commit*> missingCommits;
    const Commit* commit = graph->_versionController->_head.load();

    while (commit && commit->hash().get() != lastDumpedHash) {
        missingCommits.emplace_back(commit);
        commit = commit->getPreviousCommit();
    }

    // The log ends on a commit the history in memory does not reach, so appending to it would
    // describe a history that never existed
    if (!commit) {
        return DumpError::result(DumpErrorType::COMMIT_HASH_NOT_FOUND);
    }

    if (missingCommits.empty()) {
        return {};
    }

    const StdPath graphDirectory {graphDir.get()};

    for (const Commit* missingCommit : missingCommits | std::views::reverse) {
        const fs::Path commitDir = graphDir / "commits" / std::to_string(missingCommit->hash().get());

        if (auto res = removeInterruptedDump(*missingCommit, graphDirectory); !res) {
            return res;
        }

        if (auto res = CommitDumper::dump(*missingCommit, commitDir, partsDir); !res) {
            return res;
        }

        hashes.emplace_back(missingCommit->hash().get());
    }

    const StdPath preparedLog = graphDirectory / "commitlog.tmp";
    std::error_code error;

    std::filesystem::remove(preparedLog, error);
    if (auto res = CommitLogFile::write(fs::Path {preparedLog.string()}, hashes); !res) {
        return res;
    }

    std::filesystem::rename(preparedLog, graphDirectory / "commitlog", error);
    if (error) {
        return DumpError::result(DumpErrorType::CANNOT_REPLACE_COMMIT_LOG);
    }

    return {};
}
