#include <gtest/gtest.h>
#include <cstdlib>
#include <string>

TEST(ClusterIntegrationTest, RunClusterScript) {
    // We assume we are running from the build directory, so we navigate up to run the script.
    // Or we just invoke it if we are in the root directory.
    
    // Check if the script exists in the current directory (if ctest is run from root)
    // or from project root. We'll try to find it.
    std::string script_path = "./scripts/run_cluster_test.sh";
    
    int ret = std::system(script_path.c_str());
    if (ret != 0) {
        // Fallback for when running from build/debug/
        script_path = "../../scripts/run_cluster_test.sh";
        ret = std::system(script_path.c_str());
    }
    
    EXPECT_EQ(ret, 0) << "Cluster integration script failed!";
}
