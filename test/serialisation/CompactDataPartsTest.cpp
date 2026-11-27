#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include <spdlog/fmt/fmt.h>

#include "Graph.h"
#include "NLOutputSink.h"
#include "QueryConfig.h"
#include "SystemManager.h"
#include "TuringDB.h"
#include "dump/GraphLoader.h"
#include "versioning/Change.h"
#include "versioning/ChangeID.h"
#include "versioning/CommitHash.h"
#include "versioning/VersionController.h"

#include "IRTestRows.h"
#include "TuringTest.h"
#include "TuringTestEnv.h"

using namespace db;
using namespace turing::test;

// Submits one change per person, so the graph ends up with one commit and one data part per name
class CompactDataPartsTest : public TuringTest {
public:
    void initialize() override {
        _turingDir = fs::Path {_outDir} / "turing";
        _graphDir = _turingDir / "graphs" / _graphName;
        _env = TuringTestEnv::createSyncedOnDisk(_turingDir);

        {
            SystemAccessor system = _env->getSystemManager().accessUnique();
            ASSERT_TRUE(system.createGraph(_graphName));
        }

        for (const std::string_view name : {"Alice", "Bob", "Carol", "Dave"}) {
            submitPerson(name);
        }
    }

protected:
    std::string _graphName = "compactdataparts";
    fs::Path _turingDir;
    fs::Path _graphDir;
    std::unique_ptr<TuringTestEnv> _env;
    QueryConfig _queryConfig;

    QueryStatus query(std::string_view queryText, ChangeID change = ChangeID::head(), NLOutputSink* sink = nullptr) {
        NullSink nullSink;
        const QueryState state(_graphName,
                               &_env->getMem(),
                               &_env->getCompilerContext(),
                               &_queryConfig,
                               sink ? sink : &nullSink,
                               CommitHash::head(),
                               change);

        return _env->getDB().query(queryText, state);
    }

    void submitPerson(std::string_view name) {
        submit(fmt::format(R"(CREATE (n:Person {{name: "{}"}}))", name));
    }

    // Runs @param queryText in a change of its own, so it lands in a data part of its own
    void submit(std::string_view queryText) {
        ChangeID change = ChangeID::head();
        {
            SystemAccessor system = _env->getSystemManager().accessUnique();
            const auto result = system.newChange(_graphName, CommitHash::head());
            ASSERT_TRUE(result);
            change = result.value()->id();
        }

        const QueryStatus status = query(queryText, change);
        ASSERT_TRUE(status.isOk()) << status.getError();
        ASSERT_TRUE(query("CHANGE SUBMIT", change).isOk());
    }

    Rows rowsOf(std::string_view queryText) {
        RowSink sink;
        const QueryStatus status = query(queryText, ChangeID::head(), &sink);
        EXPECT_TRUE(status.isOk()) << status.getError();

        Rows rows;
        sink.sortedRows(rows);

        return rows;
    }

    // Every one of @param count rows holds @param value
    static Rows rowsOfValue(size_t count, std::string_view value) {
        return Rows(count, Row {std::string {value}});
    }

    size_t countEntries(std::string_view directoryName) const {
        const std::filesystem::path directory = std::filesystem::path {_graphDir.get()} / directoryName;

        return std::distance(std::filesystem::directory_iterator {directory},
                             std::filesystem::directory_iterator {});
    }

    bool entryExists(std::string_view directoryName, std::string_view entryName) const {
        return std::filesystem::exists(std::filesystem::path {_graphDir.get()} / directoryName / entryName);
    }

    void plantOrphan(std::string_view directoryName, std::string_view entryName) const {
        const std::filesystem::path orphan = std::filesystem::path {_graphDir.get()} / directoryName / entryName;

        std::filesystem::create_directories(orphan);
        std::ofstream {orphan / "padding"} << "unreferenced";
    }

    void restartServer() {
        _env.reset();
        _env = TuringTestEnv::createSyncedOnDisk(_turingDir);

        SystemAccessor system = _env->getSystemManager().accessUnique();
        ASSERT_TRUE(system.loadGraph(_graphName));
    }

    void expectPeople() {
        RowSink sink;
        const QueryStatus status = query("MATCH (n:Person) RETURN n.name", ChangeID::head(), &sink);
        ASSERT_TRUE(status.isOk()) << status.getError();

        Rows rows;
        sink.sortedRows(rows);

        const Rows expected {{"Alice"}, {"Bob"}, {"Carol"}, {"Dave"}};
        EXPECT_EQ(expected, rows);
    }
};

TEST_F(CompactDataPartsTest, mergeDataPartsReachesDisk) {
    const size_t commitsBefore = countEntries("commits");
    const size_t partsBefore = countEntries("dataparts");

    ASSERT_TRUE(query("MERGE_DATAPARTS").isOk());

    EXPECT_EQ(commitsBefore + 1, countEntries("commits"));
    EXPECT_EQ(partsBefore + 1, countEntries("dataparts"));

    restartServer();
    expectPeople();
}

TEST_F(CompactDataPartsTest, squashingAcrossAMergeKeepsTheMergedPartOnly) {
    ASSERT_TRUE(query("MERGE_DATAPARTS").isOk());
    ASSERT_GT(countEntries("dataparts"), 1);

    const QueryStatus status = query("SQUASH COMMITS");
    ASSERT_TRUE(status.isOk()) << status.getError();

    EXPECT_EQ(1, countEntries("commits"));
    EXPECT_EQ(1, countEntries("dataparts"));
    expectPeople();

    restartServer();
    expectPeople();
}

TEST_F(CompactDataPartsTest, aCommitAfterTheMergeSquashesWithIt) {
    ASSERT_TRUE(query("MERGE_DATAPARTS").isOk());
    submitPerson("Erin");

    ASSERT_TRUE(query("SQUASH COMMITS").isOk());

    EXPECT_EQ(1, countEntries("commits"));
    EXPECT_EQ(2, countEntries("dataparts"));

    restartServer();

    RowSink sink;
    ASSERT_TRUE(query("MATCH (n:Person) RETURN n.name", ChangeID::head(), &sink).isOk());

    Rows rows;
    sink.sortedRows(rows);
    EXPECT_EQ(5, rows.size());
}

TEST_F(CompactDataPartsTest, squashingPrunesWhatNoCommitLogEntryReferences) {
    plantOrphan("commits", "999");
    plantOrphan("dataparts", "888");

    ASSERT_TRUE(query("SQUASH COMMITS").isOk());

    EXPECT_FALSE(entryExists("commits", "999"));
    EXPECT_FALSE(entryExists("dataparts", "888"));
    EXPECT_EQ(1, countEntries("commits"));
    expectPeople();
}

TEST_F(CompactDataPartsTest, aSquashThatChangesNothingStillPrunes) {
    ASSERT_TRUE(query("SQUASH COMMITS").isOk());

    plantOrphan("commits", "999");
    plantOrphan("dataparts", "888");

    ASSERT_TRUE(query("SQUASH COMMITS").isOk());

    EXPECT_FALSE(entryExists("commits", "999"));
    EXPECT_FALSE(entryExists("dataparts", "888"));
}

TEST_F(CompactDataPartsTest, theCompactedGraphAcceptsNewCommits) {
    ASSERT_TRUE(query("MERGE_DATAPARTS").isOk());
    ASSERT_TRUE(query("SQUASH COMMITS").isOk());

    submitPerson("Erin");
    restartServer();

    RowSink sink;
    ASSERT_TRUE(query("MATCH (n:Person) RETURN n.name", ChangeID::head(), &sink).isOk());

    Rows rows;
    sink.sortedRows(rows);
    EXPECT_EQ(5, rows.size());
}

TEST_F(CompactDataPartsTest, squashingAloneKeepsEveryDataPart) {
    const size_t partsBefore = countEntries("dataparts");
    ASSERT_GT(partsBefore, 1);

    ASSERT_TRUE(query("SQUASH COMMITS").isOk());

    EXPECT_EQ(1, countEntries("commits"));
    EXPECT_EQ(partsBefore, countEntries("dataparts"));
}

TEST_F(CompactDataPartsTest, anOpenChangeOnAnotherGraphDoesNotBlockTheMerge) {
    {
        SystemAccessor system = _env->getSystemManager().accessUnique();
        ASSERT_TRUE(system.createGraph("othergraph"));
        ASSERT_TRUE(system.newChange("othergraph", CommitHash::head()));
    }

    const QueryStatus status = query("MERGE_DATAPARTS");
    ASSERT_TRUE(status.isOk()) << status.getError();

    restartServer();
    expectPeople();
}

TEST_F(CompactDataPartsTest, anOpenChangeOnTheMergedGraphBlocksTheMerge) {
    {
        SystemAccessor system = _env->getSystemManager().accessUnique();
        ASSERT_TRUE(system.newChange(_graphName, CommitHash::head()));
    }

    const size_t commitsBefore = countEntries("commits");

    EXPECT_FALSE(query("MERGE_DATAPARTS").isOk());
    EXPECT_EQ(commitsBefore, countEntries("commits"));
}

// More records than the sort keeps in insertion order, each updated in several data parts:
// the merged part must hold the value a read returned before the merge, the newest one
TEST_F(CompactDataPartsTest, mergeKeepsTheNewestValueOfAnUpdatedNodeProperty) {
    constexpr size_t RECORDS = 40;
    constexpr size_t UPDATES = 6;

    std::string create = "CREATE ";
    for (size_t i = 0; i < RECORDS; i++) {
        create += fmt::format(R"({}(:Record {{key: {}, value: "rev0"}}))", i ? ", " : "", i);
    }
    submit(create);

    for (size_t update = 1; update <= UPDATES; update++) {
        submit(fmt::format(R"(MATCH (r:Record) SET r.value = "rev{}")", update));
    }

    const Rows expected = rowsOfValue(RECORDS, fmt::format("rev{}", UPDATES));
    ASSERT_EQ(expected, rowsOf("MATCH (r:Record) RETURN r.value"));

    ASSERT_TRUE(query("MERGE_DATAPARTS").isOk());
    EXPECT_EQ(expected, rowsOf("MATCH (r:Record) RETURN r.value"));

    restartServer();
    EXPECT_EQ(expected, rowsOf("MATCH (r:Record) RETURN r.value"));
}

TEST_F(CompactDataPartsTest, mergeKeepsTheNewestValueOfAnUpdatedEdgeProperty) {
    constexpr size_t LINKS = 40;
    constexpr size_t UPDATES = 6;

    std::string create = "CREATE ";
    for (size_t i = 0; i < LINKS; i++) {
        create += fmt::format(R"({}(:From {{key: {}}})-[:Link {{value: "rev0"}}]->(:To {{key: {}}}))", i ? ", " : "", i, i);
    }
    submit(create);

    for (size_t update = 1; update <= UPDATES; update++) {
        submit(fmt::format(R"(MATCH (:From)-[l:Link]->(:To) SET l.value = "rev{}")", update));
    }

    const Rows expected = rowsOfValue(LINKS, fmt::format("rev{}", UPDATES));
    ASSERT_EQ(expected, rowsOf("MATCH (:From)-[l:Link]->(:To) RETURN l.value"));

    ASSERT_TRUE(query("MERGE_DATAPARTS").isOk());
    EXPECT_EQ(expected, rowsOf("MATCH (:From)-[l:Link]->(:To) RETURN l.value"));

    restartServer();
    EXPECT_EQ(expected, rowsOf("MATCH (:From)-[l:Link]->(:To) RETURN l.value"));
}

// A property set to null in a newer data part hides the older value; the merge must not bring it back
TEST_F(CompactDataPartsTest, mergeKeepsAPropertySetToNullCleared) {
    submit(R"(CREATE (:Record {key: 1, value: "old"}), (:Record {key: 2, value: "kept"}))");
    submit(R"(MATCH (r:Record {key: 1}) SET r.value = null)");

    const Rows cleared = rowsOf("MATCH (r:Record {key: 1}) RETURN r.value");
    ASSERT_EQ(1, cleared.size());
    ASSERT_NE(Row {"old"}, cleared.front());

    ASSERT_TRUE(query("MERGE_DATAPARTS").isOk());
    EXPECT_EQ(cleared, rowsOf("MATCH (r:Record {key: 1}) RETURN r.value"));
    EXPECT_EQ(Rows {{"kept"}}, rowsOf("MATCH (r:Record {key: 2}) RETURN r.value"));

    restartServer();
    EXPECT_EQ(cleared, rowsOf("MATCH (r:Record {key: 1}) RETURN r.value"));
    EXPECT_EQ(Rows {{"kept"}}, rowsOf("MATCH (r:Record {key: 2}) RETURN r.value"));
}

int main(int argc, char** argv) {
    return turing::test::turingTestMain(argc, argv, [] {
        testing::GTEST_FLAG(repeat) = 1;
    });
}
