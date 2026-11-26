#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include <spdlog/fmt/fmt.h>

#include "Graph.h"
#include "GraphCompactor.h"
#include "NLOutputSink.h"
#include "QueryConfig.h"
#include "SystemManager.h"
#include "TuringDB.h"
#include "metadata/DateTime.h"
#include "metadata/Duration.h"
#include "versioning/Commit.h"
#include "versioning/CommitData.h"
#include "versioning/VersionController.h"

#include "IRTestRows.h"
#include "TuringTest.h"
#include "TuringTestEnv.h"

using namespace db;
using namespace turing::test;

class GraphCompactorTest : public TuringTest {
public:
    void initialize() override {
        _turingDir = fs::Path {_outDir} / "turing";
        _env = TuringTestEnv::createSyncedOnDisk(_turingDir);
        createGraph(_graphName);
    }

protected:
    static constexpr int64_t MICROSECONDS_PER_DAY = 24ll * 3600 * 1000 * 1000;
    static constexpr Duration RETENTION {7 * MICROSECONDS_PER_DAY};

    std::string _graphName = "compacted";
    fs::Path _turingDir;
    std::unique_ptr<TuringTestEnv> _env;
    QueryConfig _queryConfig;

    static DateTime daysFromNow(int64_t days) {
        return DateTime::now() + Duration {days * MICROSECONDS_PER_DAY};
    }

    void createGraph(std::string_view graphName) {
        SystemAccessor system = _env->getSystemManager().accessUnique();
        ASSERT_TRUE(system.createGraph(graphName));
    }

    ChangeID newChange(std::string_view graphName, CommitHash base = CommitHash::head()) {
        SystemAccessor system = _env->getSystemManager().accessUnique();
        const auto result = system.newChange(graphName, base);
        EXPECT_TRUE(result);

        return result.value()->id();
    }

    QueryStatus query(std::string_view graphName, std::string_view queryText, ChangeID change = ChangeID::head(), NLOutputSink* sink = nullptr) {
        NullSink nullSink;
        const QueryState state(graphName,
                               &_env->getMem(),
                               &_env->getCompilerContext(),
                               &_queryConfig,
                               sink ? sink : &nullSink,
                               CommitHash::head(),
                               change);

        return _env->getDB().query(queryText, state);
    }

    void submitPeople(std::string_view graphName, size_t count) {
        for (size_t index = 0; index < count; index++) {
            const ChangeID change = newChange(graphName);
            ASSERT_TRUE(query(graphName, fmt::format(R"(CREATE (n:Person {{name: "P{}"}}))", index), change).isOk());
            ASSERT_TRUE(query(graphName, "CHANGE SUBMIT", change).isOk());
        }
    }

    void compact(DateTime now) {
        GraphCompactor compactor(&_env->getSystemManager(), RETENTION);
        compactor.compactGraphs(now);
    }

    size_t countCommits(std::string_view graphName) {
        SystemAccessor system = _env->getSystemManager().accessUnique();

        return system.getGraph(graphName)->getVersionController().getNumCommits();
    }

    CommitHash getHeadHash(std::string_view graphName) {
        SystemAccessor system = _env->getSystemManager().accessUnique();

        return system.getGraph(graphName)->getHeadHash();
    }

    size_t countHeadDataParts(std::string_view graphName) {
        SystemAccessor system = _env->getSystemManager().accessUnique();
        const Graph* graph = system.getGraph(graphName);
        const VersionController& versions = graph->getVersionController();

        return versions.getCommitSafe(graph->getHeadHash())->data().allDataparts().size();
    }

    size_t countPeople(std::string_view graphName) {
        RowSink sink;
        const QueryStatus status = query(graphName, "MATCH (n:Person) RETURN n.name", ChangeID::head(), &sink);
        EXPECT_TRUE(status.isOk()) << status.getError();

        Rows rows;
        sink.sortedRows(rows);

        return rows.size();
    }

    void restartServer() {
        _env.reset();
        _env = TuringTestEnv::createSyncedOnDisk(_turingDir);

        SystemAccessor system = _env->getSystemManager().accessUnique();
        ASSERT_TRUE(system.loadGraph(_graphName));
    }
};

TEST_F(GraphCompactorTest, squashesTheCommitsOlderThanTheRetention) {
    submitPeople(_graphName, 4);
    ASSERT_EQ(countCommits(_graphName), 5);

    compact(daysFromNow(8));

    EXPECT_EQ(countCommits(_graphName), 1);
    EXPECT_EQ(countPeople(_graphName), 4);

    restartServer();
    EXPECT_EQ(countCommits(_graphName), 1);
    EXPECT_EQ(countPeople(_graphName), 4);
}

TEST_F(GraphCompactorTest, keepsTheCommitsInsideTheRetention) {
    submitPeople(_graphName, 4);

    compact(daysFromNow(6));

    EXPECT_EQ(countCommits(_graphName), 5);
}

TEST_F(GraphCompactorTest, commitTimesSurviveARestart) {
    submitPeople(_graphName, 4);
    restartServer();

    compact(daysFromNow(6));
    EXPECT_EQ(countCommits(_graphName), 5);

    compact(daysFromNow(8));
    EXPECT_EQ(countCommits(_graphName), 1);
    EXPECT_EQ(countPeople(_graphName), 4);
}

TEST_F(GraphCompactorTest, mergesTheDataPartsPastTheThreshold) {
    submitPeople(_graphName, GraphCompactor::MERGE_DATA_PART_COUNT + 1);
    ASSERT_GT(countHeadDataParts(_graphName), GraphCompactor::MERGE_DATA_PART_COUNT);

    compact(daysFromNow(0));

    EXPECT_EQ(countHeadDataParts(_graphName), 1);
    EXPECT_EQ(countPeople(_graphName), GraphCompactor::MERGE_DATA_PART_COUNT + 1);

    restartServer();
    EXPECT_EQ(countHeadDataParts(_graphName), 1);
    EXPECT_EQ(countPeople(_graphName), GraphCompactor::MERGE_DATA_PART_COUNT + 1);
}

TEST_F(GraphCompactorTest, leavesFewDataPartsUnmerged) {
    submitPeople(_graphName, 3);
    const size_t partsBefore = countHeadDataParts(_graphName);
    const size_t commitsBefore = countCommits(_graphName);

    compact(daysFromNow(0));

    EXPECT_EQ(countHeadDataParts(_graphName), partsBefore);
    EXPECT_EQ(countCommits(_graphName), commitsBefore);
}

TEST_F(GraphCompactorTest, aGraphWithAnOpenChangeOnAnOldCommitIsLeftAndTheOthersAreCompacted) {
    const std::string otherGraph = "otherGraph";
    createGraph(otherGraph);
    submitPeople(_graphName, 1);
    const CommitHash oldCommit = getHeadHash(_graphName);
    submitPeople(_graphName, 2);
    submitPeople(otherGraph, 3);

    const ChangeID openChange = newChange(_graphName, oldCommit);

    compact(daysFromNow(8));

    EXPECT_EQ(countCommits(_graphName), 4);
    EXPECT_EQ(countCommits(otherGraph), 1);
    EXPECT_EQ(countPeople(otherGraph), 3);

    ASSERT_TRUE(query(_graphName, "CHANGE SUBMIT", openChange).isOk());
}

TEST_F(GraphCompactorTest, anOpenChangeOnTheHeadStillSubmitsAfterTheSquash) {
    submitPeople(_graphName, 3);
    const ChangeID openChange = newChange(_graphName);
    ASSERT_TRUE(query(_graphName, R"(CREATE (n:Person {name: "Late"}))", openChange).isOk());

    compact(daysFromNow(8));

    EXPECT_EQ(countCommits(_graphName), 1);
    ASSERT_TRUE(query(_graphName, "CHANGE SUBMIT", openChange).isOk());
    EXPECT_EQ(countPeople(_graphName), 4);
}

TEST_F(GraphCompactorTest, aRunningCompactorStopsWithTheServer) {
    GraphCompactor compactor(&_env->getSystemManager(), RETENTION);
    compactor.start(Duration {3600ll * 1000 * 1000});
    compactor.stop();
}

int main(int argc, char** argv) {
    return turingTestMain(argc, argv);
}
