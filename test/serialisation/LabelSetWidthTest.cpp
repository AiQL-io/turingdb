#include <gtest/gtest.h>

#include <vector>

#include "TuringTest.h"

#include "FilePageReader.h"
#include "FilePageWriter.h"
#include "dump/DumpConfig.h"
#include "dump/GraphDumpHelper.h"
#include "dump/LabelSetMapDumper.h"
#include "dump/LabelSetMapLoader.h"
#include "metadata/LabelSetMap.h"

using namespace db;
using namespace turing::test;

namespace {

// The labels of every label set in @param labelsets, in the order they were loaded
std::vector<std::vector<LabelID>> labelsOf(const LabelSetMap& labelsets) {
    std::vector<std::vector<LabelID>> all;

    for (size_t i = 0; i < labelsets.getCount(); i++) {
        const auto handle = labelsets.getValue(LabelSetID(i));
        EXPECT_TRUE(handle.has_value());

        std::vector<LabelID> labels;
        LabelSet::fromIntegers(handle->integers()).decompose(labels);
        all.push_back(std::move(labels));
    }

    return all;
}

}

class LabelSetWidthTest : public TuringTest {
protected:
    fs::Path path() const { return fs::Path {_outDir} / "labelsets"; }

    // Writes a label set file as a build whose label sets had @param words words would, holding @param sets
    void writeWithWidth(uint64_t words, const std::vector<std::vector<uint64_t>>& sets) {
        auto writer = fs::FilePageWriter::open(path(), DumpConfig::PAGE_SIZE);
        ASSERT_TRUE(writer);

        GraphDumpHelper::writeFileHeader(writer.value());
        writer->writeToCurrentPage((uint64_t)LabelSet::IntegerSize);
        writer->writeToCurrentPage(words);
        writer->writeToCurrentPage((uint64_t)sets.size());
        writer->writeToCurrentPage((uint64_t)1);

        writer->nextPage();
        writer->writeToCurrentPage((uint64_t)sets.size());
        for (const auto& set : sets) {
            ASSERT_EQ(words, set.size());
            for (const uint64_t word : set) {
                writer->writeToCurrentPage(word);
            }
        }

        writer->finish();
    }

    DumpResult<void> load(LabelSetMap& labelsets) {
        auto reader = fs::FilePageReader::open(path(), DumpConfig::PAGE_SIZE);
        EXPECT_TRUE(reader);

        return LabelSetMapLoader(reader.value()).load(labelsets);
    }
};

TEST_F(LabelSetWidthTest, aGraphHoldsMoreThan256Labels) {
    EXPECT_EQ(4096, LabelSet::BitCount);

    LabelSet labelset;
    labelset.set(LabelID(255));
    labelset.set(LabelID(256));
    labelset.set(LabelID(4095));

    EXPECT_TRUE(labelset.hasLabel(LabelID(256)));
    EXPECT_TRUE(labelset.hasLabel(LabelID(4095)));
    EXPECT_FALSE(labelset.hasLabel(LabelID(257)));
    EXPECT_EQ(3, labelset.size());
}

// A graph written before label sets were widened loads with the words it lacks left empty
TEST_F(LabelSetWidthTest, labelSetsWrittenFourWordsWideLoad) {
    writeWithWidth(4, {{0b101, 0, 0, 0}, {0, 1, 0, 1ull << 63}});

    LabelSetMap labelsets;
    const auto res = load(labelsets);
    ASSERT_TRUE(res) << res.error().fmtMessage();

    const std::vector<std::vector<LabelID>> expected {{LabelID(0), LabelID(2)}, {LabelID(64), LabelID(255)}};
    EXPECT_EQ(expected, labelsOf(labelsets));
}

TEST_F(LabelSetWidthTest, labelSetsAboveTheOldCapacityRoundTrip) {
    LabelSetMap written;
    written.getOrCreate(LabelSet::fromList({LabelID(1), LabelID(300)}));
    written.getOrCreate(LabelSet::fromList({LabelID(4000)}));

    {
        auto writer = fs::FilePageWriter::open(path(), DumpConfig::PAGE_SIZE);
        ASSERT_TRUE(writer);
        ASSERT_TRUE(LabelSetMapDumper(writer.value()).dump(written));
        writer->finish();
    }

    LabelSetMap loaded;
    const auto res = load(loaded);
    ASSERT_TRUE(res) << res.error().fmtMessage();

    EXPECT_EQ(labelsOf(written), labelsOf(loaded));
}

TEST_F(LabelSetWidthTest, labelSetsWiderThanThisBuildAreRefused) {
    writeWithWidth(LabelSet::IntegerCount + 1, {std::vector<uint64_t>(LabelSet::IntegerCount + 1, 0)});

    LabelSetMap labelsets;
    EXPECT_FALSE(load(labelsets));
}

int main(int argc, char** argv) {
    return turing::test::turingTestMain(argc, argv, [] {
        testing::GTEST_FLAG(repeat) = 1;
    });
}
