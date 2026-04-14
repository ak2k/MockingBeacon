/*
 * BabbleSim MCUmgr SMP echo test
 *
 * Device 0 (smp_server): connectable BLE peripheral with MCUmgr SMP service
 * Device 1 (smp_client): connects, sends SMP echo, verifies response
 */

#include <zephyr/kernel.h>
#include "bs_tracing.h"
#include "bstests.h"
#include "babblekit/testcase.h"

extern void entrypoint_smp_server(void);
extern void entrypoint_smp_client(void);
extern enum bst_result_t bst_result;

static void test_end_cb(void)
{
    if (bst_result != Passed) {
        TEST_PRINT("Test has not passed.");
    }
}

static const struct bst_test_instance entrypoints[] = {
    {
        .test_id = "smp_server",
        .test_delete_f = test_end_cb,
        .test_main_f = entrypoint_smp_server,
    },
    {
        .test_id = "smp_client",
        .test_delete_f = test_end_cb,
        .test_main_f = entrypoint_smp_client,
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
