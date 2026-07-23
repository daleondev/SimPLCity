#pragma once

#include <fx_api.h>

#include <cstdint>

namespace runtime::storage::detail
{
    inline constexpr std::uint32_t sd_data_crc_error{ 0x0000'0002U };

    [[nodiscard]] constexpr auto should_retry_sd_mount_in_one_bit(std::uint8_t bus_width,
                                                                  UINT filex_status,
                                                                  std::uint32_t sd_error) noexcept -> bool
    {
        return bus_width == 4U && filex_status == FX_BOOT_ERROR && (sd_error & sd_data_crc_error) != 0U;
    }
}
