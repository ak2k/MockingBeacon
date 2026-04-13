/*
 * BabbleSim BLE advertisement test for Everytag beacon
 *
 * Two devices:
 * - Device 0 (advertiser): broadcasts AirTag and FMDN advertisements
 *   using the C++ beacon_logic functions, rotates keys
 * - Device 1 (scanner): receives and verifies the exact advertisement
 *   payload bytes match expected values
 */

#include <zephyr/kernel.h>
#include "bs_tracing.h"
#include "bstests.h"
#include "babblekit/testcase.h"

extern void entrypoint_advertiser(void);
extern void entrypoint_scanner(void);
extern enum bst_result_t bst_result;

static void test_end_cb(void)
{
    if (bst_result != Passed) {
        TEST_PRINT("Test has not passed.");
    }
}

static const struct bst_test_instance entrypoints[] = {
    {
        .test_id = "advertiser",
        .test_delete_f = test_end_cb,
        .test_main_f = entrypoint_advertiser,
    },
    {
        .test_id = "scanner",
        .test_delete_f = test_end_cb,
        .test_main_f = entrypoint_scanner,
    },
    BSTEST_END_MARKER,
};

static struct bst_test_list *install(struct bst_test_list *tests)
{
    return bst_add_tests(tests, entrypoints);
}

bst_test_install_t test_installers[] = {install, NULL};

int main(void)
{
    bst_main();
    return 0;
}
