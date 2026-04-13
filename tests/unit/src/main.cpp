#include <zephyr/ztest.h>

ZTEST_SUITE(beacon_smoke, NULL, NULL, NULL, NULL, NULL);

ZTEST(beacon_smoke, test_placeholder)
{
    zassert_true(true, "Placeholder test passes");
}
