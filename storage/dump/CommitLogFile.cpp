#include "CommitLogFile.h"

#include "DumpConfig.h"
#include "FileReader.h"
#include "FileWriter.h"
#include "GraphDumpHelper.h"

using namespace db;

DumpResult<void> CommitLogFile::read(const fs::Path& logFile, std::vector<uint64_t>& hashes) {
    auto fileRes = fs::File::open(logFile);
    if (!fileRes) {
        return DumpError::result(DumpErrorType::CANNOT_OPEN_COMMIT_LOG, fileRes.error());
    }

    fs::FileReader reader;
    reader.setFile(&fileRes.value());
    reader.read();
    if (reader.errorOccured()) {
        return DumpError::result(DumpErrorType::CANNOT_OPEN_COMMIT_LOG);
    }

    fs::ByteBufferIterator it = reader.iterateBuffer();

    const auto fileHeaderRes = GraphDumpHelper::checkFileHeader(&it);
    if (!fileHeaderRes) {
        return fileHeaderRes.get_unexpected();
    }

    const size_t numEntries = it.get<uint64_t>();

    const uint64_t expectedSize = DumpConfig::FILE_HEADER_STRIDE + sizeof(uint64_t) * (numEntries + 1);
    const uint64_t actualSize = reader.file().getInfo()._size;
    if (expectedSize != actualSize) {
        return DumpError::result(DumpErrorType::COULD_NOT_READ_COMMIT_LOG);
    }

    hashes.clear();
    for (size_t i = 0; i < numEntries; i++) {
        hashes.emplace_back(it.get<uint64_t>());
    }

    return {};
}

DumpResult<void> CommitLogFile::write(const fs::Path& logFile, const std::vector<uint64_t>& hashes) {
    auto fileRes = fs::File::createAndOpen(logFile);
    if (!fileRes) {
        return DumpError::result(DumpErrorType::CANNOT_OPEN_COMMIT_LOG, fileRes.error());
    }

    fs::FileWriter<> writer;
    writer.setFile(&fileRes.value());

    GraphDumpHelper::writeFileHeader(&writer);
    writer.write(hashes.size());
    for (const uint64_t hash : hashes) {
        writer.write(hash);
    }

    writer.flush();
    if (writer.errorOccured()) {
        return DumpError::result(DumpErrorType::COULD_NOT_WRITE_COMMIT_LOG, *writer.error());
    }

    return {};
}
