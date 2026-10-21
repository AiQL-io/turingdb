#include <gtest/gtest.h>

#include "IRTestRows.h"
#include "WriteQueryTest.h"

using namespace db;
using namespace turing::test;

class NotOfANullBooleanConversionTest : public WriteQueryTest {
};

TEST_F(NotOfANullBooleanConversionTest, notOfAConversionYieldingNullIsNull) {
    expectRows("RETURN NOT toBoolean('x')", {{"null"}});
}

TEST_F(NotOfANullBooleanConversionTest, notOfAProjectedConversionYieldingNullIsNull) {
    expectRows("WITH toBoolean('x') AS b RETURN NOT b", {{"null"}});
}

TEST_F(NotOfANullBooleanConversionTest, aStringOfTheNegationIsNull) {
    expectRows("RETURN toString(NOT toBoolean('x'))", {{"null"}});
}

TEST_F(NotOfANullBooleanConversionTest, notOfAnOrNullConversionYieldingNullIsNull) {
    expectRows("RETURN NOT toBooleanOrNull('x')", {{"null"}});
}

TEST_F(NotOfANullBooleanConversionTest, notOfAConversionYieldingAValueIsItsNegation) {
    expectRows("RETURN NOT toBoolean('true'), NOT toBoolean('false')", {{"false", "true"}});
}

TEST_F(NotOfANullBooleanConversionTest, notOfAConversionPerRow) {
    expectRows("UNWIND ['true', 'x', 'false'] AS s RETURN s, NOT toBoolean(s)",
               {{"true", "false"}, {"x", "null"}, {"false", "true"}});
}

TEST_F(NotOfANullBooleanConversionTest, notOfAConversionOfAPropertyIsNull) {
    expectRows("MATCH (n:Person {name: 'Remy'}) RETURN NOT toBoolean(n.name)", {{"null"}});
}

TEST_F(NotOfANullBooleanConversionTest, aFilterOnTheNegationKeepsNoRow) {
    expectRows("MATCH (n:Person) WHERE NOT toBoolean(n.name) RETURN n.name", {});
}

TEST_F(NotOfANullBooleanConversionTest, aFilterOnTheNegationOfAConstantYieldingNullKeepsNoRow) {
    expectRows("MATCH (n:Person) WHERE NOT toBoolean('x') RETURN n.name", {});
}

TEST_F(NotOfANullBooleanConversionTest, aFilterOnAConstantConversionYieldingNullKeepsNoRow) {
    expectRows("MATCH (n:Person) WHERE toBoolean('x') RETURN n.name", {});
}

TEST_F(NotOfANullBooleanConversionTest, aFilterOnTheNegationOfAFalseConversionKeepsEveryRow) {
    expectRows("MATCH (n:Person) WHERE NOT toBoolean('false') RETURN n.name",
               {{"Adam"}, {"Cyrus"}, {"Doruk"}, {"Luc"}, {"Martina"}, {"Maxime"}, {"Remy"}, {"Suhas"}});
}

TEST_F(NotOfANullBooleanConversionTest, aFilterOverASingleRowOnTheNegation) {
    expectRows("WITH 1 AS x WHERE NOT toBoolean('x') RETURN x", {});
    expectRows("WITH 1 AS x WHERE NOT toBoolean('false') RETURN x", {{"1"}});
}

TEST_F(NotOfANullBooleanConversionTest, aCaseOnTheNegationFallsThrough) {
    expectRows("UNWIND [0, 1] AS i RETURN i, CASE WHEN NOT toBoolean('x') THEN 'a' WHEN true THEN 'b' END",
               {{"0", "b"}, {"1", "b"}});
}

int main(int argc, char** argv) {
    return turing::test::turingTestMain(argc, argv);
}
