#include <gtest/gtest.h>

#include "WriteQueryTest.h"

using namespace db;
using namespace turing::test;

// An expression over constants alone that may raise, as size('ab') - 1 may, is computed
// over the rows that reach it, so after a clause that dedups, filters or aggregates them it
// yields one value per row kept, beside the columns of those rows.
class RaisingConstantOverReducedRowsTest : public WriteQueryTest {
};

TEST_F(RaisingConstantOverReducedRowsTest, returnsOneValuePerDistinctValue) {
    expectRows("UNWIND [1, 1, 2] AS x WITH DISTINCT x RETURN size('ab') - 1", {{"1"}, {"1"}});
    expectRows("UNWIND [1, 1, 2] AS x WITH DISTINCT x RETURN toInteger('3') + 1", {{"4"}, {"4"}});
}

TEST_F(RaisingConstantOverReducedRowsTest, returnsOneValuePerDistinctEndNode) {
    expectRows("MATCH (a)-->(b) WITH DISTINCT b RETURN size('ab') - 1", Rows(12, {"1"}));
}

TEST_F(RaisingConstantOverReducedRowsTest, returnsTheValueBesideEachDistinctValue) {
    expectRows("UNWIND [1, 1, 2] AS x WITH DISTINCT x RETURN x, size('ab') - 1", {{"1", "1"}, {"2", "1"}});
}

TEST_F(RaisingConstantOverReducedRowsTest, filtersTheDistinctValuesOnTheValue) {
    expectRows("UNWIND [1, 1, 2] AS x WITH DISTINCT x WHERE size('ab') - 1 > 0 RETURN x", {{"1"}, {"2"}});
}

TEST_F(RaisingConstantOverReducedRowsTest, returnsNoValueWhenTheOnlyRowIsFiltered) {
    expectRows("WITH 1 AS a WHERE a > 5 RETURN size('ab') - 1", {});
}

TEST_F(RaisingConstantOverReducedRowsTest, groupsTheDistinctValuesByTheValue) {
    expectRows("UNWIND [1, 1, 2] AS x WITH DISTINCT x RETURN size('ab') - 1 AS k, count(*) AS c", {{"1", "2"}});
}

TEST_F(RaisingConstantOverReducedRowsTest, publishesTheValueBesideEachDistinctValue) {
    expectRows("UNWIND [1, 1, 2] AS x WITH DISTINCT x WITH x, size('ab') - 1 AS s RETURN x, s", {{"1", "1"}, {"2", "1"}});
}

TEST_F(RaisingConstantOverReducedRowsTest, unwindsAListOfTheValuePerDistinctValue) {
    expectRows("UNWIND [1, 1, 2] AS x WITH DISTINCT x UNWIND [size('ab') - 1] AS y RETURN x, y", {{"1", "1"}, {"2", "1"}});
}

TEST_F(RaisingConstantOverReducedRowsTest, unwindsARangeUpToTheValuePerDistinctValue) {
    expectRows("UNWIND [1, 1, 2] AS x WITH DISTINCT x UNWIND range(1, size('ab') - 1) AS y RETURN x, y", {{"1", "1"}, {"2", "1"}});
}

TEST_F(RaisingConstantOverReducedRowsTest, mapsAListToTheValuePerDistinctValue) {
    expectRows("UNWIND [1, 1, 2] AS x WITH DISTINCT x RETURN x, [y IN [1] | size('ab') - 1]", {{"1", "[1]"}, {"2", "[1]"}});
}

TEST_F(RaisingConstantOverReducedRowsTest, returnsOneValuePerRowACallBodyKeeps) {
    expectRows("MATCH (n:Person) WHERE n.name = 'Remy' "
               "CALL (n) { MATCH (n)-->(m) WITH DISTINCT m.isFrench AS k RETURN size('ab') - 1 AS one } "
               "RETURN one",
               {{"1"}, {"1"}});
}

TEST_F(RaisingConstantOverReducedRowsTest, createsOneNodePerDistinctValue) {
    applyWrite("UNWIND [1, 1, 2] AS x WITH DISTINCT x CREATE (:Tally {v: size('ab') - 1})");

    expectRows("MATCH (t:Tally) RETURN t.v", {{"1"}, {"1"}});
}

int main(int argc, char** argv) {
    return turing::test::turingTestMain(argc, argv);
}
