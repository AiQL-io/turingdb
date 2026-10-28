#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>

#include "Graph.h"
#include "NLOutputSink.h"
#include "QueryConfig.h"
#include "SystemManager.h"
#include "TuringDB.h"
#include "versioning/Change.h"
#include "versioning/ChangeID.h"
#include "versioning/CommitHash.h"

#include "IRTestRows.h"
#include "TuringTest.h"
#include "TuringTestEnv.h"

using namespace db;
using namespace turing::test;

class SquashSubmitTest : public TuringTest {
public:
    void initialize() override {
        _turingDir = fs::Path {_outDir} / "turing";
        _env = TuringTestEnv::createSyncedOnDisk(_turingDir);

        SystemAccessor system = _env->getSystemManager().accessUnique();
        system.createGraph(_graphName);
    }

protected:
    std::string _graphName = "squashtest";
    fs::Path _turingDir;
    std::unique_ptr<TuringTestEnv> _env;
    QueryConfig _queryConfig;

    ChangeID newChange() {
        SystemAccessor system = _env->getSystemManager().accessUnique();
        const auto result = system.newChange(_graphName);
        EXPECT_TRUE(result);

        return result.value()->id();
    }

    QueryStatus query(std::string_view queryText, ChangeID change, NLOutputSink* sink = nullptr) {
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

    // Creates two nodes and an edge across three COMMITs on the change
    void writeThreeCommits(ChangeID change) {
        ASSERT_TRUE(query(R"(CREATE (n:Person {name: "Alice"}))", change));
        ASSERT_TRUE(query("COMMIT", change));
        ASSERT_TRUE(query(R"(CREATE (n:Person {name: "Bob"}))", change));
        ASSERT_TRUE(query("COMMIT", change));
        ASSERT_TRUE(query(R"(MATCH (a:Person {name: "Alice"}), (b:Person {name: "Bob"}) CREATE (a)-[:KNOWS]->(b))", change));
        ASSERT_TRUE(query("COMMIT", change));
    }

    size_t countCommitDirectories() const {
        const std::filesystem::path commitsDirectory = std::filesystem::path {_turingDir.get()} / "graphs" / _graphName / "commits";

        return std::distance(std::filesystem::directory_iterator {commitsDirectory},
                             std::filesystem::directory_iterator {});
    }

    void collectRows(std::string_view queryText, ChangeID change, Rows& rows) {
        RowSink sink;
        const QueryStatus status = query(queryText, change, &sink);
        ASSERT_TRUE(status.isOk()) << status.getError();

        sink.sortedRows(rows);
    }

    void expectKnowsPairs(const Rows& expected) {
        Rows rows;
        collectRows("MATCH (a:Person)-[:KNOWS]->(b:Person) RETURN a.name, b.name", ChangeID::head(), rows);

        EXPECT_EQ(expected, rows);
    }
};

TEST_F(SquashSubmitTest, squashWritesOneCommit) {
    const size_t commitsBefore = countCommitDirectories();

    const ChangeID change = newChange();
    writeThreeCommits(change);

    Rows personIDsOnBranch;
    collectRows("MATCH (n:Person) RETURN n", change, personIDsOnBranch);
    ASSERT_EQ(2, personIDsOnBranch.size());

    ASSERT_TRUE(query("CHANGE SUBMIT SQUASH", change));

    EXPECT_EQ(commitsBefore + 1, countCommitDirectories());
    expectKnowsPairs({{"Alice", "Bob"}});

    // Squashing keeps the ids the change gave its entities
    Rows personIDsOnMain;
    collectRows("MATCH (n:Person) RETURN n", ChangeID::head(), personIDsOnMain);
    EXPECT_EQ(personIDsOnBranch, personIDsOnMain);
}

TEST_F(SquashSubmitTest, mergeKeepsEveryCommit) {
    const size_t commitsBefore = countCommitDirectories();

    const ChangeID change = newChange();
    writeThreeCommits(change);

    ASSERT_TRUE(query("CHANGE SUBMIT", change));

    // Three COMMITs plus the commit holding whatever was written after the last one
    EXPECT_EQ(commitsBefore + 4, countCommitDirectories());
    expectKnowsPairs({{"Alice", "Bob"}});
}

TEST_F(SquashSubmitTest, squashOfEmptyChangeWritesOneCommit) {
    const size_t commitsBefore = countCommitDirectories();

    const ChangeID change = newChange();
    ASSERT_TRUE(query("CHANGE SUBMIT SQUASH", change));

    EXPECT_EQ(commitsBefore + 1, countCommitDirectories());
}

TEST_F(SquashSubmitTest, squashOfSingleCommitMatchesMerge) {
    const size_t commitsBefore = countCommitDirectories();

    const ChangeID change = newChange();
    ASSERT_TRUE(query(R"(CREATE (n:Person {name: "Alice"}))", change));
    ASSERT_TRUE(query("CHANGE SUBMIT SQUASH", change));

    EXPECT_EQ(commitsBefore + 1, countCommitDirectories());

    Rows people;
    collectRows("MATCH (n:Person) RETURN n", ChangeID::head(), people);
    EXPECT_EQ(1, people.size());
}

TEST_F(SquashSubmitTest, squashSubmitsOnTopOfNewerMain) {
    const ChangeID squashed = newChange();
    const ChangeID other = newChange();

    ASSERT_TRUE(query(R"(CREATE (n:Company {name: "Acme"}))", other));
    ASSERT_TRUE(query("CHANGE SUBMIT", other));

    writeThreeCommits(squashed);
    ASSERT_TRUE(query("CHANGE SUBMIT SQUASH", squashed));

    expectKnowsPairs({{"Alice", "Bob"}});

    Rows companies;
    collectRows("MATCH (n:Company) RETURN n", ChangeID::head(), companies);
    EXPECT_EQ(1, companies.size());
}

TEST_F(SquashSubmitTest, conflictFailsAndLeavesChangeOpen) {
    const ChangeID setup = newChange();
    ASSERT_TRUE(query("CREATE (n:Source)-[e:NEWEDGE]->(m:Target)", setup));
    ASSERT_TRUE(query("CHANGE SUBMIT", setup));

    const ChangeID squashed = newChange();
    const ChangeID other = newChange();

    ASSERT_TRUE(query("MATCH (n:Source) DETACH DELETE n", squashed));
    ASSERT_TRUE(query("MATCH (n:Target) DETACH DELETE n", other));
    ASSERT_TRUE(query("CHANGE SUBMIT", other));

    const size_t commitsBefore = countCommitDirectories();

    EXPECT_FALSE(query("CHANGE SUBMIT SQUASH", squashed).isOk());
    EXPECT_EQ(commitsBefore, countCommitDirectories());

    // The change was not dropped
    EXPECT_TRUE(query("MATCH (n:Source) RETURN n", squashed).isOk());
}

TEST_F(SquashSubmitTest, mergeDataPartsBlockedByOpenChangeThenSucceedsAfterSquash) {
    const ChangeID change = newChange();
    writeThreeCommits(change);

    EXPECT_FALSE(query("MERGE_DATAPARTS", ChangeID::head()).isOk());

    ASSERT_TRUE(query("CHANGE SUBMIT SQUASH", change));

    const QueryStatus merged = query("MERGE_DATAPARTS", ChangeID::head());
    EXPECT_TRUE(merged.isOk()) << merged.getError();

    expectKnowsPairs({{"Alice", "Bob"}});
}

int main(int argc, char** argv) {
    return turing::test::turingTestMain(argc, argv, [] {
        testing::GTEST_FLAG(repeat) = 1;
    });
}
