#include "libc/storage_media_policy.hpp"

#include <gtest/gtest.h>

namespace
{
    using runtime::storage::detail::sd_data_crc_error;
    using runtime::storage::detail::should_retry_sd_mount_in_one_bit;
}

TEST(StorageMediaPolicy, RetriesOnlyFourBitBootReadsThatFailCrcValidation)
{
    EXPECT_TRUE(should_retry_sd_mount_in_one_bit(4U, FX_BOOT_ERROR, sd_data_crc_error));
    EXPECT_TRUE(should_retry_sd_mount_in_one_bit(4U, FX_BOOT_ERROR, sd_data_crc_error | 0x80U));

    EXPECT_FALSE(should_retry_sd_mount_in_one_bit(1U, FX_BOOT_ERROR, sd_data_crc_error));
    EXPECT_FALSE(should_retry_sd_mount_in_one_bit(4U, FX_SUCCESS, sd_data_crc_error));
    EXPECT_FALSE(should_retry_sd_mount_in_one_bit(4U, FX_MEDIA_INVALID, sd_data_crc_error));
    EXPECT_FALSE(should_retry_sd_mount_in_one_bit(4U, FX_BOOT_ERROR, 0U));
}
