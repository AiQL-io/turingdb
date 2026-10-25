#include <gtest/gtest.h>

#include <string_view>

#include "CallV3Test.h"
#include "IRTestRows.h"

using namespace turing::test;

class AnyConcatenationOverNoRowTest : public CallV3Test {
protected:
    void expectRows(std::string_view query, const Rows& expected) {
        RowSink sink;
        runQuery(query, sink);

        Rows rows;
        sink.sortedRows(rows);

        EXPECT_EQ(rows, expected) << query;
    }
};

TEST_F(AnyConcatenationOverNoRowTest, concatenatesNothingWhereNoRowReaches) {
    expectRows("MATCH (p:Person) WHERE p.name = 'Nobody' RETURN [1, 'a'][0] || '!'", {});
}

TEST_F(AnyConcatenationOverNoRowTest, concatenatesNothingOverAnEmptyUnwind) {
    expectRows("UNWIND [] AS x RETURN [1, 'a'][0] || '!'", {});
}

TEST_F(AnyConcatenationOverNoRowTest, concatenatesTheCellOfARowReachingIt) {
    expectRows("MATCH (p:Person) WHERE p.name = 'Remy' RETURN [1, 'a'][1] || '!'", {{"a!"}});
}

int main(int argc, char** argv) {
    return turing::test::turingTestMain(argc, argv);
}
