#include "HistorySquashDumper.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include "CommitJournalDumper.h"
#include "CommitLogFile.h"
#include "CommitMetaDataDumper.h"
#include "DumpConfig.h"
#include "FilePageWriter.h"
#include "FileReader.h"
#include "FileWriter.h"
#include "GraphDumpHelper.h"
#include "versioning/Commit.h"
#include "versioning/CommitJournal.h"

#include "Profiler.h"

using namespace db;

namespace {

using StdPath = std::filesystem::path;

constexpr std::string_view preparedSuffix = ".squash";
constexpr std::string_view replacedSuffix = ".replaced";

bool hasSuffix(const std::string& text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Rewrites the two files of a commit directory the squash changes: the counts and data part
// sizes in its metadata, and the writes of its journal
DumpResult<void> rewriteCommitFiles(const Commit& commit, const StdPath& commitDirectory) {
    {
        const StdPath metadataFile = commitDirectory / "metadata";
        std::filesystem::remove(metadataFile);

        auto fileRes = fs::File::createAndOpen(fs::Path {metadataFile.string()});
        if (!fileRes) {
            return DumpError::result(DumpErrorType::CANNOT_OPEN_COMMIT_METADATA, fileRes.error());
        }

        fs::FileWriter writer;
        writer.setFile(&fileRes.value());

        if (auto res = CommitMetaDataDumper::dump(commit, writer); !res) {
            return res;
        }

        writer.flush();
        writer.file().close();
        if (writer.errorOccured()) {
            return DumpError::result(DumpErrorType::COULD_NOT_WRITE_COMMIT_METADATA, *writer.error());
        }
    }

    {
        const StdPath journalFile = commitDirectory / "journal";
        std::filesystem::remove(journalFile);

        auto writerRes = fs::FilePageWriter::open(fs::Path {journalFile.string()}, DumpConfig::PAGE_SIZE);
        if (!writerRes) {
            return DumpError::result(DumpErrorType::CANNOT_OPEN_JOURNAL, writerRes.error());
        }

        CommitJournalDumper dumper(writerRes.value());
        if (auto res = dumper.dump(commit.history().journal()); !res) {
            return res;
        }
    }

    return {};
}

// Puts the prepared commit and commit log in place of the ones they replace
DumpResult<void> rollForward(const StdPath& graphDirectory) {
    const StdPath commitsDirectory = graphDirectory / "commits";
    const StdPath preparedLog = graphDirectory / "commitlog.squash";

    std::vector<StdPath> preparedDirectories;
    std::vector<StdPath> replacedDirectories;

    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator {commitsDirectory}) {
        const std::string name = entry.path().filename().string();

        if (hasSuffix(name, preparedSuffix)) {
            preparedDirectories.emplace_back(entry.path());
        }
    }

    std::error_code error;

    for (const StdPath& preparedDirectory : preparedDirectories) {
        const std::string preparedName = preparedDirectory.filename().string();
        const std::string tipName = preparedName.substr(0, preparedName.size() - preparedSuffix.size());
        const StdPath tipDirectory = commitsDirectory / tipName;
        const StdPath replacedDirectory = commitsDirectory / (tipName + std::string {replacedSuffix});

        // A crash between the two renames leaves the old directory under its replaced name
        if (std::filesystem::exists(tipDirectory)) {
            std::filesystem::remove_all(replacedDirectory, error);
            std::filesystem::rename(tipDirectory, replacedDirectory, error);
            if (error) {
                return DumpError::result(DumpErrorType::CANNOT_REPLACE_COMMIT);
            }
        }

        std::filesystem::rename(preparedDirectory, tipDirectory, error);
        if (error) {
            return DumpError::result(DumpErrorType::CANNOT_REPLACE_COMMIT);
        }
    }

    std::filesystem::rename(preparedLog, graphDirectory / "commitlog", error);
    if (error) {
        return DumpError::result(DumpErrorType::CANNOT_REPLACE_COMMIT_LOG);
    }

    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator {commitsDirectory}) {
        const std::string name = entry.path().filename().string();

        if (hasSuffix(name, replacedSuffix)) {
            replacedDirectories.emplace_back(entry.path());
        }
    }

    for (const StdPath& replacedDirectory : replacedDirectories) {
        std::filesystem::remove_all(replacedDirectory, error);
    }

    return {};
}

// Throws away what a squash prepared before it reached the point of no return
void discardPrepared(const StdPath& graphDirectory) {
    const StdPath commitsDirectory = graphDirectory / "commits";
    std::error_code error;

    std::vector<StdPath> preparedDirectories;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator {commitsDirectory, error}) {
        if (hasSuffix(entry.path().filename().string(), preparedSuffix)) {
            preparedDirectories.emplace_back(entry.path());
        }
    }

    for (const StdPath& preparedDirectory : preparedDirectories) {
        std::filesystem::remove_all(preparedDirectory, error);
    }

    std::filesystem::remove(graphDirectory / "commitlog.squash.tmp", error);
    std::filesystem::remove(graphDirectory / "commitlog.squash", error);
}

}

DumpResult<void> HistorySquashDumper::dump(const Commit& tip,
                                           const fs::Path& graphDir,
                                           std::span<const CommitHash> squashed) {
    Profile profile("HistorySquashDumper::dump");

    const StdPath graphDirectory {graphDir.get()};
    const StdPath commitsDirectory = graphDirectory / "commits";
    const std::string tipName = std::to_string(tip.hash().get());
    const StdPath tipDirectory = commitsDirectory / tipName;
    const StdPath preparedDirectory = commitsDirectory / (tipName + std::string {preparedSuffix});
    const fs::Path preparedLogTemp {(graphDirectory / "commitlog.squash.tmp").string()};
    const StdPath preparedLog = graphDirectory / "commitlog.squash";

    if (!std::filesystem::exists(tipDirectory)) {
        return DumpError::result(DumpErrorType::COMMIT_HASH_NOT_FOUND);
    }

    discardPrepared(graphDirectory);

    // Everything up to the rename of the commit log into place can be thrown away
    std::error_code error;
    std::filesystem::copy(tipDirectory, preparedDirectory, std::filesystem::copy_options::recursive, error);
    if (error) {
        discardPrepared(graphDirectory);
        return DumpError::result(DumpErrorType::CANNOT_COPY_COMMIT);
    }

    if (auto res = rewriteCommitFiles(tip, preparedDirectory); !res) {
        discardPrepared(graphDirectory);
        return res;
    }

    std::vector<uint64_t> hashes;
    if (auto res = CommitLogFile::read(fs::Path {(graphDirectory / "commitlog").string()}, hashes); !res) {
        discardPrepared(graphDirectory);
        return res;
    }

    std::vector<uint64_t> remainingHashes;
    for (const uint64_t hash : hashes) {
        const bool isSquashed = std::find_if(squashed.begin(), squashed.end(), [hash](const CommitHash& other) {
                                    return other.get() == hash;
                                }) != squashed.end();

        if (!isSquashed) {
            remainingHashes.emplace_back(hash);
        }
    }

    if (auto res = CommitLogFile::write(preparedLogTemp, remainingHashes); !res) {
        discardPrepared(graphDirectory);
        return res;
    }

    // From here on a crash is finished by recover
    std::filesystem::rename(preparedLogTemp.get(), preparedLog, error);
    if (error) {
        discardPrepared(graphDirectory);
        return DumpError::result(DumpErrorType::CANNOT_REPLACE_COMMIT_LOG);
    }

    if (auto res = rollForward(graphDirectory); !res) {
        return res;
    }

    // The commits the squash dropped are only reachable through the old commit log
    for (const CommitHash hash : squashed) {
        std::filesystem::remove_all(commitsDirectory / std::to_string(hash.get()), error);
    }

    return {};
}

DumpResult<void> HistorySquashDumper::recover(const fs::Path& graphDir) {
    const StdPath graphDirectory {graphDir.get()};

    if (!std::filesystem::exists(graphDirectory / "commits")) {
        return {};
    }

    if (std::filesystem::exists(graphDirectory / "commitlog.squash")) {
        spdlog::warn("Finishing a commit history squash that was interrupted in {}", graphDir.get());
        return rollForward(graphDirectory);
    }

    discardPrepared(graphDirectory);

    return {};
}
