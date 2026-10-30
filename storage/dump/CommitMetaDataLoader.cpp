#include "CommitMetaDataLoader.h"

#include "FileReader.h"
#include "GraphDumpHelper.h"

using namespace db;

DumpResult<void> CommitMetaDataLoader::loadDataPartIDs(const fs::Path& metadataFile,
                                                       size_t& numCommitDataParts,
                                                       std::vector<uint64_t>& dataPartIDs) {
    auto fileRes = fs::File::open(metadataFile);
    if (!fileRes) {
        return DumpError::result(DumpErrorType::CANNOT_OPEN_COMMIT_METADATA, fileRes.error());
    }

    fs::FileReader reader;
    reader.setFile(&fileRes.value());

    reader.read();
    if (reader.errorOccured()) {
        return DumpError::result(DumpErrorType::COULD_NOT_READ_COMMIT_METADATA, *reader.error());
    }

    fs::ByteBufferIterator it = reader.iterateBuffer();

    if (auto res = GraphDumpHelper::checkFileHeader(&it); !res) {
        return res.get_unexpected();
    }

    // Skip the number of nodes and edges
    it.advance(2 * sizeof(uint64_t));

    numCommitDataParts = it.get<uint64_t>();
    const size_t numAllDataParts = it.get<uint64_t>();

    dataPartIDs.clear();
    for (size_t i = 0; i < numAllDataParts; i++) {
        dataPartIDs.emplace_back(it.get<uint64_t>());
    }

    reader.file().close();

    return {};
}
