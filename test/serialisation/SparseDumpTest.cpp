#include <gtest/gtest.h>

#include <filesystem>

#include "FileHoles.h"
#include "Graph.h"
#include "Path.h"
#include "SimpleGraph.h"
#include "SystemManager.h"
#include "TuringTest.h"
#include "TuringTestEnv.h"
#include "comparators/GraphComparator.h"
#include "dump/GraphDumper.h"
#include "dump/GraphLoader.h"

using namespace db;
using namespace turing::test;

class SparseDumpTest : public TuringTest {
public:
    void initialize() override {
        _env = TuringTestEnv::createSyncedOnDisk(fs::Path {_outDir} / "turing");
        SystemAccessor system = _env->getSystemManager().accessUnique();
        _graph = system.createGraph("simple");
        SimpleGraph::createSimpleGraph(_graph);
        _dumpPath = fs::Path {_outDir + "/dump"};
    }

protected:
    std::unique_ptr<TuringTestEnv> _env;
    Graph* _graph {nullptr};
    fs::Path _dumpPath;

    void measure(size_t& apparentBytes, size_t& allocatedBytes) const {
        apparentBytes = 0;
        allocatedBytes = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(_dumpPath.get())) {
            if (!entry.is_regular_file()) {
                continue;
            }

            apparentBytes += static_cast<size_t>(entry.file_size());
            allocatedBytes += getAllocatedBytes(entry.path().string());
        }
    }
};

TEST_F(SparseDumpTest, aDumpAllocatesOnlyTheBytesItWrites) {
    ASSERT_TRUE(GraphDumper::dump(_graph, _dumpPath));

    size_t apparentBytes = 0;
    size_t allocatedBytes = 0;
    measure(apparentBytes, allocatedBytes);

    ASSERT_GT(apparentBytes, 0);
    if (directoryKeepsHoles(_outDir)) {
        EXPECT_LT(allocatedBytes * 20, apparentBytes) << allocatedBytes << " of " << apparentBytes << " bytes allocated";
    }

    std::unique_ptr<Graph> loaded = Graph::create();
    ASSERT_TRUE(GraphLoader::load(loaded.get(), _dumpPath));
    EXPECT_TRUE(GraphComparator::same(*_graph, *loaded));
}

int main(int argc, char** argv) {
    return turingTestMain(argc, argv);
}
