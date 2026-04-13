// Host-native test runner for pure C++ modules
// Uses simple assert-based testing (no Zephyr dependency)

#include <cassert>
#include <cstdio>
#include <cstring>

// Test infrastructure
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name)                                                                                 \
    static void test_##name();                                                                     \
    static struct TestReg_##name {                                                                  \
        TestReg_##name() { test_##name(); }                                                        \
    } test_reg_##name;                                                                             \
    static void test_##name()

#define ASSERT_TRUE(cond)                                                                          \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (!(cond)) {                                                                             \
            printf("  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond);                              \
            return;                                                                                \
        }                                                                                          \
        tests_passed++;                                                                            \
    } while (0)

#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_NE(a, b) ASSERT_TRUE((a) != (b))
#define ASSERT_MEM_EQ(a, b, len) ASSERT_TRUE(memcmp((a), (b), (len)) == 0)

// Placeholder test
TEST(smoke)
{
    ASSERT_TRUE(true);
    printf("  smoke test passed\n");
}

int main()
{
    printf("Running host tests...\n");
    // Tests auto-register via static constructors
    printf("\n%d/%d assertions passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
