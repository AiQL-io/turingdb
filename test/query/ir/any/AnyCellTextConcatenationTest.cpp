#include <gtest/gtest.h>

#include <string_view>

#include "CallV3Test.h"

using namespace turing::test;

class AnyCellTextConcatenationTest : public CallV3Test {
protected:
    void expectError(std::string_view query) {
        runQueryExpectingError(query, "Operands are not valid and compatible types for '||'");
    }
};

TEST_F(AnyCellTextConcatenationTest, joinsNoNumberCellsAsText) {
    expectError("WITH [1, 2, 'a'] AS xs RETURN xs[0] || xs[1], xs[2] || xs[0]");
}

TEST_F(AnyCellTextConcatenationTest, joinsNoListCellToANumberCell) {
    expectError("WITH [[1], 2, 'a'] AS xs RETURN xs[0] || xs[1]");
}

int main(int argc, char** argv) {
    return turing::test::turingTestMain(argc, argv);
}
