# SPDX-License-Identifier: Apache-2.0

if(CONFIG_SOC_NRF54L05_CPUAPP)
  board_runner_args(jlink "--device=nRF54L05_M33" "--speed=4000")
endif()

# 25055 SWD header has no separate reset line — use system reset.
board_runner_args(nrfutil "--softreset")

include(${ZEPHYR_BASE}/boards/common/nrfutil.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
