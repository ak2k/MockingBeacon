#!/usr/bin/env bash
# BLE advertisement + key rotation test
set -eu

: "${ZEPHYR_BASE:?ZEPHYR_BASE must be set}"
: "${BSIM_OUT_PATH:?BSIM_OUT_PATH must be set}"

source ${ZEPHYR_BASE}/tests/bsim/sh_common.source

simulation_id="everytag_adv_test"
verbosity_level=2

# 5 seconds should be enough for 3 key rotations at 500ms each
SIM_LEN_US=$((5 * 1000 * 1000))

test_exe="${BSIM_OUT_PATH}/bin/bs_nrf52_bsim_tests_bsim_prj_conf"

cd ${BSIM_OUT_PATH}/bin

Execute "${test_exe}" -v=${verbosity_level} -s=${simulation_id} -d=0 -rs=420 -testid=advertiser
Execute "${test_exe}" -v=${verbosity_level} -s=${simulation_id} -d=1 -rs=69  -testid=scanner

Execute ./bs_2G4_phy_v1 -v=${verbosity_level} -s=${simulation_id} -D=2 -sim_length=${SIM_LEN_US} $@

wait_for_background_jobs
