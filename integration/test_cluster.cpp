#include <gtest/gtest.h>
#include <cstdlib>
#include <string>
#ifndef _WIN32
#include <sys/wait.h>
#endif

// Paths are injected by CMake so the test does not depend on the working
// directory ctest happens to run it from.
#ifndef MEMINFO_CLUSTER_SCRIPT
#error "MEMINFO_CLUSTER_SCRIPT must be defined by the build"
#endif
#ifndef MEMINFO_BUILD_DIR
#error "MEMINFO_BUILD_DIR must be defined by the build"
#endif

namespace {

// std::system returns a wait status, not an exit code: a script that exits 127
// yields 32512. Decode it so failures report the real code.
int exit_code_of(int wait_status) {
#ifdef _WIN32
    return wait_status;
#else
    if (WIFEXITED(wait_status)) return WEXITSTATUS(wait_status);
    if (WIFSIGNALED(wait_status)) return 128 + WTERMSIG(wait_status);
    return wait_status;
#endif
}

} // namespace

TEST(ClusterIntegrationTest, RunClusterScript) {
#ifdef _WIN32
    // The harness is a bash script driven through std::system(), which runs
    // cmd.exe here: the `VAR=value command` prefix, the script itself, and its
    // job-control based daemon teardown all have no cmd equivalent. It also
    // addresses binaries as <build>/<target>/<name>, which only holds for a
    // single-config generator -- MSVC puts them under <build>/<target>/Debug/.
    // Skip rather than report a failure that says nothing about the code.
    GTEST_SKIP() << "Cluster harness is a POSIX shell script; not runnable on Windows";
#else
    // Point the script at the binaries this build actually produced rather than
    // letting it guess a build directory.
    std::string command = std::string("MEMINFO_BUILD_DIR='") + MEMINFO_BUILD_DIR +
                          "' '" + MEMINFO_CLUSTER_SCRIPT + "'";

    int status = std::system(command.c_str());
    ASSERT_NE(status, -1) << "Failed to launch the cluster script";

    EXPECT_EQ(exit_code_of(status), 0)
        << "Cluster integration script failed (" << MEMINFO_CLUSTER_SCRIPT << ")";
#endif
}
