#include <gtest/gtest.h>

#include "IRTestRows.h"
#include "WriteQueryTest.h"

using namespace db;
using namespace turing::test;

class OrNullOfAVariableLengthPathTest : public WriteQueryTest {
};

TEST_F(OrNullOfAVariableLengthPathTest, eachConversionOfARelationshipListIsNull) {
    const std::vector<std::pair<std::string, size_t>> pathsPerPerson = {
        {"Adam", 7}, {"Cyrus", 2}, {"Doruk", 1}, {"Luc", 2},
        {"Martina", 1}, {"Maxime", 2}, {"Remy", 8}, {"Suhas", 2},
    };

    Rows expected;
    for (const auto& [name, pathCount] : pathsPerPerson) {
        expected.insert(expected.end(), pathCount, Row {name, "null", "null", "null", "null"});
    }

    expectRows("MATCH (a:Person)-[e*1..2]->(b) "
               "RETURN a.name, toStringOrNull(e), toIntegerOrNull(e), toFloatOrNull(e), toBooleanOrNull(e)",
               expected);
}

TEST_F(OrNullOfAVariableLengthPathTest, aConversionBesideTheHopCount) {
    expectRows("MATCH (a:Person {name: 'Remy'})-[e*1..2]->(b) RETURN size(e), toStringOrNull(e)",
               {{"1", "null"}, {"1", "null"}, {"1", "null"}, {"1", "null"},
                {"2", "null"}, {"2", "null"}, {"2", "null"}, {"2", "null"}});
}

TEST_F(OrNullOfAVariableLengthPathTest, aConversionOfAnOptionalRelationshipList) {
    expectRows("MATCH (a:Person {name: 'Doruk'}) OPTIONAL MATCH (a)-[e*2..2]->(b) RETURN a.name, toStringOrNull(e)",
               {{"Doruk", "null"}});
}

TEST_F(OrNullOfAVariableLengthPathTest, aConversionOfANodeGroupIsNull) {
    expectRows("MATCH (a:Person {name: 'Remy'}) ((x)-->(y)){1,1} (b) RETURN b.name, toStringOrNull(x), toIntegerOrNull(y)",
               {{"Adam", "null", "null"}, {"Computers", "null", "null"},
                {"Eighties", "null", "null"}, {"Ghosts", "null", "null"}});
}

TEST_F(OrNullOfAVariableLengthPathTest, theStrictConversionOfARelationshipListIsATypeError) {
    expectError("MATCH (a:Person)-[e*1..2]->(b) RETURN toString(e)", "Invalid arguments for function 'toString'");
}

int main(int argc, char** argv) {
    return turing::test::turingTestMain(argc, argv);
}
