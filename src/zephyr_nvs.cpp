// zephyr_nvs.cpp -- INvsStorage implementation wrapping Zephyr ZMS
#include "zephyr_nvs.hpp"

#ifndef HOST_TEST

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/zms.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>

#define ZMS_PARTITION storage_partition
#define ZMS_PARTITION_DEVICE FIXED_PARTITION_DEVICE(ZMS_PARTITION)
#define ZMS_PARTITION_OFFSET FIXED_PARTITION_OFFSET(ZMS_PARTITION)

namespace beacon {

int ZephyrNvsStorage::init() {
    struct flash_pages_info info;
    int rc;

    fs_.flash_device = ZMS_PARTITION_DEVICE;
    if (!device_is_ready(fs_.flash_device)) {
        printk("Storage device %s is not ready\n", fs_.flash_device->name);
        return -1;
    }
    fs_.offset = ZMS_PARTITION_OFFSET;
    rc = flash_get_page_info_by_offs(fs_.flash_device, fs_.offset, &info);
    if (rc) {
        printk("Unable to get page info\n");
        return -1;
    }
    printk("ZMS sector size %d, offset %lx\n", info.size, info.start_offset);
    fs_.sector_size = info.size;
    fs_.sector_count = 3U;

    rc = zms_mount(&fs_);
    if (rc) {
        printk("Storage init failed\n");
        return -1;
    }
    mounted_ = true;
    printk("ZMS initialized\n");
    return 0;
}

int ZephyrNvsStorage::read(uint16_t id, void* data, size_t len) {
    if (!mounted_) {
        return -1;
    }
    return zms_read(&fs_, id, data, len);
}

int ZephyrNvsStorage::write(uint16_t id, const void* data, size_t len) {
    if (!mounted_) {
        return -1;
    }
    return zms_write(&fs_, id, data, len);
}

} // namespace beacon

#endif // !HOST_TEST
