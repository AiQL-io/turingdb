#include <fstream>
#include <iterator>
#include <vector>

#include "FileHoles.h"
#include "TuringTest.h"

#include "FilePageWriter.h"
#include "Path.h"

using namespace turing::test;

class FilePageWriterSparseTest : public TuringTest {
protected:
    static constexpr size_t PAGE_SIZE = fs::DEFAULT_PAGE_SIZE;
    static constexpr size_t MAX_ALLOCATED_BYTES = 64 * 1024;

    void initialize() override {
        _path = fs::Path {_outDir + "/testfile"};
    }

    fs::FilePageWriter openWriter() {
        auto writer = fs::FilePageWriter::open(_path);
        if (!writer) {
            throw std::runtime_error("Could not open " + _path.get());
        }

        return std::move(writer.value());
    }

    void readFile(std::vector<uint8_t>& bytes) const {
        std::ifstream file(_path.get(), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char> {file}, std::istreambuf_iterator<char> {});
    }

    void expectHoles() const {
        if (directoryKeepsHoles(_outDir)) {
            EXPECT_LE(getAllocatedBytes(_path.get()), MAX_ALLOCATED_BYTES);
        }
    }

    static void expectBytes(const std::vector<uint8_t>& bytes, size_t begin, size_t end, uint8_t value) {
        for (size_t index = begin; index < end; index++) {
            ASSERT_EQ(bytes[index], value) << "at byte " << index;
        }
    }

    fs::Path _path;
};

TEST_F(FilePageWriterSparseTest, aPartialPageKeepsItsSizeButNotItsBlocks) {
    {
        fs::FilePageWriter writer = openWriter();
        const std::vector<uint8_t> data(13, 1);
        writer.write(std::span {data});
        writer.finish();
        ASSERT_FALSE(writer.errorOccured());
    }

    std::vector<uint8_t> bytes;
    readFile(bytes);

    ASSERT_EQ(bytes.size(), PAGE_SIZE);
    expectBytes(bytes, 0, 13, 1);
    expectBytes(bytes, 13, PAGE_SIZE, 0);
    expectHoles();
}

TEST_F(FilePageWriterSparseTest, emptyPagesBecomeHolesUpToTheLastOne) {
    {
        fs::FilePageWriter writer = openWriter();
        writer.nextPage();
        const std::vector<uint8_t> data(8, 7);
        writer.write(std::span {data});
        writer.nextPage();
        writer.nextPage();
        writer.finish();
        ASSERT_FALSE(writer.errorOccured());
    }

    std::vector<uint8_t> bytes;
    readFile(bytes);

    ASSERT_EQ(bytes.size(), 3 * PAGE_SIZE);
    expectBytes(bytes, 0, PAGE_SIZE, 0);
    expectBytes(bytes, PAGE_SIZE, PAGE_SIZE + 8, 7);
    expectBytes(bytes, PAGE_SIZE + 8, 3 * PAGE_SIZE, 0);
    expectHoles();
}

TEST_F(FilePageWriterSparseTest, rewritingTheFirstPageKeepsTheOthers) {
    {
        fs::FilePageWriter writer = openWriter();
        writer.nextPage();
        const std::vector<uint8_t> body(5, 9);
        writer.write(std::span {body});

        writer.seek(0);
        const std::vector<uint8_t> header(4, 3);
        writer.write(std::span {header});
        writer.finish();
        ASSERT_FALSE(writer.errorOccured());
    }

    std::vector<uint8_t> bytes;
    readFile(bytes);

    ASSERT_EQ(bytes.size(), 2 * PAGE_SIZE);
    expectBytes(bytes, 0, 4, 3);
    expectBytes(bytes, 4, PAGE_SIZE, 0);
    expectBytes(bytes, PAGE_SIZE, PAGE_SIZE + 5, 9);
    expectBytes(bytes, PAGE_SIZE + 5, 2 * PAGE_SIZE, 0);
    expectHoles();
}

TEST_F(FilePageWriterSparseTest, fullPagesAreWrittenWhole) {
    {
        fs::FilePageWriter writer = openWriter();
        const std::vector<uint8_t> data(2 * PAGE_SIZE, 5);
        writer.write(std::span {data});
        writer.finish();
        ASSERT_FALSE(writer.errorOccured());
    }

    std::vector<uint8_t> bytes;
    readFile(bytes);

    ASSERT_EQ(bytes.size(), 2 * PAGE_SIZE);
    expectBytes(bytes, 0, 2 * PAGE_SIZE, 5);
}

int main(int argc, char** argv) {
    return turingTestMain(argc, argv);
}
