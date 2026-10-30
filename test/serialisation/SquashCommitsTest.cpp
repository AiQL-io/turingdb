#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

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

// Submits one change per person, so the graph ends up with one commit per name on top of the
// empty first commit: commits[0] is the first commit, commits[1] holds Alice, and so on.
class SquashCommitsTest : public TuringTest {
public:
    void initialize() override {
        _turingDir = fs::Path {_outDir} / "turing";
        _graphDir = _turingDir / "graphs" / _graphName;
        _env = TuringTestEnv::createSyncedOnDisk(_turingDir);

        {
            SystemAccessor system = _env->getSystemManager().accessUnique();
            Graph* graph = system.createGraph(_graphName);
            _commits.push_back(graph->getHeadHash());
        }

        for (const std::string_view name : {"Alice", "Bob", "Carol", "Dave"}) {
            submitPerson(name);
        }
    }

protected:
    std::string _graphName = "squashhistory";
    fs::Path _turingDir;
    fs::Path _graphDir;
    std::unique_ptr<TuringTestEnv> _env;
    QueryConfig _queryConfig;
    std::vector<CommitHash> _commits;

    ChangeID newChange(CommitHash base = CommitHash::head()) {
        SystemAccessor system = _env->getSystemManager().accessUnique();
        const auto result = system.newChange(_graphName, base);
        EXPECT_TRUE(result);

        return result.value()->id();
    }

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
        const ChangeID change = newChange();
        ASSERT_TRUE(query(fmt::format(R"(CREATE (n:Person {{name: "{}"}}))", name), change).isOk());
        ASSERT_TRUE(query("CHANGE SUBMIT", change).isOk());

        SystemAccessor system = _env->getSystemManager().accessUnique();
        _commits.push_back(system.getGraph(_graphName)->getHeadHash());
    }

    std::string hashText(size_t commitIndex) const {
        return fmt::format("{}", _commits[commitIndex]);
    }

    QueryStatus squash(size_t baseIndex, size_t tipIndex) {
        return query(fmt::format(R"(SQUASH COMMITS AFTER "{}" THROUGH "{}")", hashText(baseIndex), hashText(tipIndex)));
    }

    size_t countCommitDirectories() const {
        const std::filesystem::path commitsDirectory = std::filesystem::path {_graphDir.get()} / "commits";

        return std::distance(std::filesystem::directory_iterator {commitsDirectory},
                             std::filesystem::directory_iterator {});
    }

    bool commitDirectoryExists(size_t commitIndex) const {
        const std::filesystem::path commitDirectory = std::filesystem::path {_graphDir.get()} / "commits" / std::to_string(_commits[commitIndex].get());

        return std::filesystem::exists(commitDirectory);
    }

    void collectRows(std::string_view queryText, Rows& rows) {
        RowSink sink;
        const QueryStatus status = query(queryText, ChangeID::head(), &sink);
        ASSERT_TRUE(status.isOk()) << status.getError();

        sink.sortedRows(rows);
    }

    void expectPeople(const Rows& expected) {
        Rows rows;
        collectRows("MATCH (n:Person) RETURN n.name", rows);

        EXPECT_EQ(expected, rows);
    }

    size_t countLoadedCommits() {
        SystemAccessor system = _env->getSystemManager().accessUnique();

        return system.getGraph(_graphName)->getVersionController().getNumCommits();
    }
};

TEST_F(SquashCommitsTest, squashesARangeIntoItsLastCommit) {
    ASSERT_EQ(5, countLoadedCommits());

    const QueryStatus status = squash(1, 3);
    ASSERT_TRUE(status.isOk()) << status.getError();

    // The first commit, Alice's commit and Dave's commit stay; Bob's and Carol's become Carol's
    EXPECT_EQ(4, countLoadedCommits());
    EXPECT_EQ(4, countCommitDirectories());

    EXPECT_TRUE(commitDirectoryExists(0));
    EXPECT_TRUE(commitDirectoryExists(1));
    EXPECT_FALSE(commitDirectoryExists(2));
    EXPECT_TRUE(commitDirectoryExists(3));
    EXPECT_TRUE(commitDirectoryExists(4));

    expectPeople({{"Alice"}, {"Bob"}, {"Carol"}, {"Dave"}});
}

TEST_F(SquashCommitsTest, entityIDsAreUntouched) {
    Rows before;
    collectRows("MATCH (n:Person) RETURN n", before);
    ASSERT_EQ(4, before.size());

    ASSERT_TRUE(squash(0, 4).isOk());

    Rows after;
    collectRows("MATCH (n:Person) RETURN n", after);
    EXPECT_EQ(before, after);
}

TEST_F(SquashCommitsTest, omittingTheBaseSquashesFromTheFirstCommit) {
    const QueryStatus status = query(fmt::format(R"(SQUASH COMMITS THROUGH "{}")", hashText(2)));
    ASSERT_TRUE(status.isOk()) << status.getError();

    // Commits 0, 1 and 2 became the second one; Carol's and Dave's stay
    EXPECT_EQ(3, countLoadedCommits());
    EXPECT_EQ(3, countCommitDirectories());
    EXPECT_FALSE(commitDirectoryExists(0));
    EXPECT_FALSE(commitDirectoryExists(1));
    EXPECT_TRUE(commitDirectoryExists(2));

    expectPeople({{"Alice"}, {"Bob"}, {"Carol"}, {"Dave"}});
}

TEST_F(SquashCommitsTest, omittingTheTipSquashesThroughTheHead) {
    const QueryStatus status = query(fmt::format(R"(SQUASH COMMITS AFTER "{}")", hashText(1)));
    ASSERT_TRUE(status.isOk()) << status.getError();

    // The first commit, Alice's commit and the squashed one
    EXPECT_EQ(3, countLoadedCommits());
    EXPECT_TRUE(commitDirectoryExists(1));
    EXPECT_FALSE(commitDirectoryExists(2));
    EXPECT_FALSE(commitDirectoryExists(3));
    EXPECT_TRUE(commitDirectoryExists(4));

    SystemAccessor system = _env->getSystemManager().accessUnique();
    EXPECT_EQ(_commits[4], system.getGraph(_graphName)->getHeadHash());
}

TEST_F(SquashCommitsTest, squashingASingleCommitChangesNothing) {
    ASSERT_TRUE(squash(2, 3).isOk());

    EXPECT_EQ(5, countLoadedCommits());
    EXPECT_EQ(5, countCommitDirectories());
}

TEST_F(SquashCommitsTest, theSquashedHistoryReloadsFromDisk) {
    ASSERT_TRUE(squash(1, 3).isOk());

    auto reloaded = Graph::create(_graphName, _graphDir);
    const auto result = GraphLoader::load(reloaded.get(), _graphDir);
    ASSERT_TRUE(result) << result.error().fmtMessage();

    EXPECT_EQ(4, reloaded->getVersionController().getNumCommits());
    EXPECT_EQ(_commits[4], reloaded->getHeadHash());
}

TEST_F(SquashCommitsTest, squashesCommitsTheServerHasNotLoaded) {
    _env.reset();
    _env = TuringTestEnv::createSyncedOnDisk(_turingDir);

    {
        SystemAccessor system = _env->getSystemManager().accessUnique();
        ASSERT_TRUE(system.loadGraph(_graphName));
    }

    // Only the head commit is in memory after a restart
    ASSERT_TRUE(squash(1, 3).isOk());

    EXPECT_EQ(4, countLoadedCommits());
    EXPECT_EQ(4, countCommitDirectories());
    expectPeople({{"Alice"}, {"Bob"}, {"Carol"}, {"Dave"}});
}

TEST_F(SquashCommitsTest, aChangeBasedBeforeTheRangeStillSubmits) {
    // Opened on Alice's commit, which the squash leaves alone
    const ChangeID change = newChange(_commits[1]);

    ASSERT_TRUE(squash(1, 3).isOk());

    ASSERT_TRUE(query(R"(CREATE (n:Person {name: "Erin"}))", change).isOk());
    ASSERT_TRUE(query("CHANGE SUBMIT", change).isOk());

    expectPeople({{"Alice"}, {"Bob"}, {"Carol"}, {"Dave"}, {"Erin"}});
}

TEST_F(SquashCommitsTest, aChangeBasedInsideTheRangeBlocksTheSquash) {
    newChange(_commits[2]);

    EXPECT_FALSE(squash(1, 3).isOk());
    EXPECT_EQ(5, countLoadedCommits());
    EXPECT_EQ(5, countCommitDirectories());
}

TEST_F(SquashCommitsTest, unknownCommitsAreRejected) {
    EXPECT_FALSE(query(R"(SQUASH COMMITS AFTER "deadbeef" THROUGH "head")").isOk());
    EXPECT_FALSE(query(fmt::format(R"(SQUASH COMMITS AFTER "{}" THROUGH "deadbeef")", hashText(1))).isOk());
    EXPECT_EQ(5, countLoadedCommits());
}

TEST_F(SquashCommitsTest, aBaseThatIsNotBeforeTheTipIsRejected) {
    EXPECT_FALSE(squash(3, 1).isOk());
    EXPECT_FALSE(squash(2, 2).isOk());
    EXPECT_EQ(5, countLoadedCommits());
}

TEST_F(SquashCommitsTest, aRangeHoldingAMergedDataPartCommitSquashesIntoOneCommit) {
    ASSERT_TRUE(query("MERGE_DATAPARTS").isOk());

    {
        SystemAccessor system = _env->getSystemManager().accessUnique();
        _commits.push_back(system.getGraph(_graphName)->getHeadHash());
    }

    const QueryStatus status = squash(1, 5);
    ASSERT_TRUE(status.isOk()) << status.getError();

    // The first commit and Alice's commit stay; Bob's through the merge become one
    EXPECT_EQ(3, countLoadedCommits());
    EXPECT_EQ(3, countCommitDirectories());
    expectPeople({{"Alice"}, {"Bob"}, {"Carol"}, {"Dave"}});
}

// The files a squash that crashed leaves behind are made by hand: the squash itself is done
// first, and then the commit log and directories are put back as the interrupted step had them.
TEST_F(SquashCommitsTest, aSquashInterruptedBeforeTheCommitLogSwapIsFinishedOnLoad) {
    namespace filesystem = std::filesystem;
    const filesystem::path graphDirectory {_graphDir.get()};

    filesystem::copy_file(graphDirectory / "commitlog", graphDirectory / "commitlog.before");
    ASSERT_TRUE(squash(1, 3).isOk());

    // Everything is in place but the new commit log, which is still waiting under its prepared name
    filesystem::rename(graphDirectory / "commitlog", graphDirectory / "commitlog.squash");
    filesystem::rename(graphDirectory / "commitlog.before", graphDirectory / "commitlog");

    auto reloaded = Graph::create(_graphName, _graphDir);
    const auto result = GraphLoader::load(reloaded.get(), _graphDir);
    ASSERT_TRUE(result) << result.error().fmtMessage();

    EXPECT_EQ(4, reloaded->getVersionController().getNumCommits());
    EXPECT_FALSE(filesystem::exists(graphDirectory / "commitlog.squash"));

    const auto loadResult = reloaded->loadCommit(_commits[3]);
    EXPECT_TRUE(loadResult) << loadResult.error().fmtMessage();
}

TEST_F(SquashCommitsTest, aSquashInterruptedBetweenTheDirectorySwapsIsFinishedOnLoad) {
    namespace filesystem = std::filesystem;
    const filesystem::path graphDirectory {_graphDir.get()};
    const filesystem::path commitsDirectory = graphDirectory / "commits";
    const std::string tipName = std::to_string(_commits[3].get());

    filesystem::copy_file(graphDirectory / "commitlog", graphDirectory / "commitlog.before");
    ASSERT_TRUE(squash(1, 3).isOk());

    // The old directory was moved aside and the prepared one was not yet moved in
    filesystem::create_directory(commitsDirectory / (tipName + ".replaced"));
    filesystem::rename(commitsDirectory / tipName, commitsDirectory / (tipName + ".squash"));
    filesystem::rename(graphDirectory / "commitlog", graphDirectory / "commitlog.squash");
    filesystem::rename(graphDirectory / "commitlog.before", graphDirectory / "commitlog");

    auto reloaded = Graph::create(_graphName, _graphDir);
    const auto result = GraphLoader::load(reloaded.get(), _graphDir);
    ASSERT_TRUE(result) << result.error().fmtMessage();

    EXPECT_EQ(4, reloaded->getVersionController().getNumCommits());
    EXPECT_FALSE(filesystem::exists(commitsDirectory / (tipName + ".squash")));
    EXPECT_FALSE(filesystem::exists(commitsDirectory / (tipName + ".replaced")));

    const auto loadResult = reloaded->loadCommit(_commits[3]);
    EXPECT_TRUE(loadResult) << loadResult.error().fmtMessage();
}

TEST_F(SquashCommitsTest, aSquashThatNeverWasPreparedIsDiscardedOnLoad) {
    namespace filesystem = std::filesystem;
    const filesystem::path graphDirectory {_graphDir.get()};
    const filesystem::path commitsDirectory = graphDirectory / "commits";
    const std::string tipName = std::to_string(_commits[3].get());

    filesystem::create_directory(commitsDirectory / (tipName + ".squash"));
    std::ofstream {graphDirectory / "commitlog.squash.tmp"} << "unfinished";

    auto reloaded = Graph::create(_graphName, _graphDir);
    const auto result = GraphLoader::load(reloaded.get(), _graphDir);
    ASSERT_TRUE(result) << result.error().fmtMessage();

    EXPECT_EQ(5, reloaded->getVersionController().getNumCommits());
    EXPECT_FALSE(filesystem::exists(commitsDirectory / (tipName + ".squash")));
    EXPECT_FALSE(filesystem::exists(graphDirectory / "commitlog.squash.tmp"));
}

int main(int argc, char** argv) {
    return turing::test::turingTestMain(argc, argv, [] {
        testing::GTEST_FLAG(repeat) = 1;
    });
}
