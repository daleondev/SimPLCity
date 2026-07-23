#!/usr/bin/env bash

set -euo pipefail

readonly PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
readonly BUILD_DIR="${PROJECT_ROOT}/build/runtime-test-stm32"
readonly FIRMWARE="${BUILD_DIR}/Application.elf"
readonly GDB_PORT="${RUNTIME_TEST_GDB_PORT:-3333}"
readonly TEST_TIMEOUT_SECONDS="${RUNTIME_TEST_TIMEOUT_SECONDS:-300}"

OPENOCD_PID=""
TEMP_DIRECTORY=""
OPENOCD_LOG=""
GDB_LOG=""

cleanup()
{
    if [[ -n "${OPENOCD_PID}" ]] && kill -0 "${OPENOCD_PID}" 2>/dev/null; then
        kill "${OPENOCD_PID}" 2>/dev/null || true
        wait "${OPENOCD_PID}" 2>/dev/null || true
    fi
    if [[ -n "${TEMP_DIRECTORY}" ]]; then
        rm -f -- "${OPENOCD_LOG}" "${GDB_LOG}"
        rmdir -- "${TEMP_DIRECTORY}" 2>/dev/null || true
    fi
}

fail()
{
    printf 'Hardware self-test infrastructure error: %s\n' "$1" >&2
    exit 2
}

require_tool()
{
    command -v "$1" >/dev/null 2>&1 || fail "required command '$1' was not found"
}

result_value()
{
    local name="$1"
    sed -n "s/^${name}=//p" "${GDB_LOG}" | tail -n 1
}

phase_name()
{
    case "$1" in
        1) printf 'library surface' ;;
        2) printf 'threads and synchronization' ;;
        3) printf 'atomics, futures, and exceptions' ;;
        4) printf 'TLS and thread-exit order' ;;
        5) printf 'libc reentrancy and entropy' ;;
        6) printf 'stream and thread stress' ;;
        7) printf 'clocks' ;;
        8) printf 'HAL timer' ;;
        9) printf 'LevelX/FileX flash standard library' ;;
        10) printf 'FileX SD standard library' ;;
        *) printf 'unknown' ;;
    esac
}

[[ "${GDB_PORT}" =~ ^[0-9]+$ ]] && ((GDB_PORT >= 1 && GDB_PORT <= 65535)) ||
    fail "RUNTIME_TEST_GDB_PORT must be an integer from 1 through 65535"
[[ "${TEST_TIMEOUT_SECONDS}" =~ ^[0-9]+$ ]] && ((TEST_TIMEOUT_SECONDS >= 1)) ||
    fail "RUNTIME_TEST_TIMEOUT_SECONDS must be a positive integer"

require_tool cmake
require_tool openocd
require_tool arm-none-eabi-gdb
require_tool timeout

cd "${PROJECT_ROOT}"

printf 'Configuring STM32 hardware self-test...\n'
cmake --preset runtime-test-stm32
printf '\nBuilding STM32 hardware self-test...\n'
cmake --build --preset runtime-test-stm32
[[ -f "${FIRMWARE}" ]] || fail "firmware was not produced at ${FIRMWARE}"

TEMP_DIRECTORY="$(mktemp -d "${TMPDIR:-/tmp}/simplcity-hardware-test.XXXXXX")"
OPENOCD_LOG="${TEMP_DIRECTORY}/openocd.log"
GDB_LOG="${TEMP_DIRECTORY}/gdb.log"
trap cleanup EXIT

printf '\nConnecting to the board...\n'
openocd \
    -f interface/stlink.cfg \
    -f target/stm32h7x_dual_bank.cfg \
    -c "gdb_port ${GDB_PORT}" \
    -c "tcl_port disabled" \
    -c "telnet_port disabled" \
    >"${OPENOCD_LOG}" 2>&1 &
OPENOCD_PID=$!

openocd_ready=false
for ((attempt = 0; attempt < 100; ++attempt)); do
    if ! kill -0 "${OPENOCD_PID}" 2>/dev/null; then
        printf '\nOpenOCD output:\n' >&2
        cat "${OPENOCD_LOG}" >&2
        fail "OpenOCD exited before accepting debugger connections"
    fi
    if grep -Fq "Listening on port ${GDB_PORT} for gdb connections" "${OPENOCD_LOG}"; then
        openocd_ready=true
        break
    fi
    sleep 0.1
done

if [[ "${openocd_ready}" != true ]]; then
    printf '\nOpenOCD output:\n' >&2
    cat "${OPENOCD_LOG}" >&2
    fail "OpenOCD did not become ready; stop any active debug session and retry"
fi

printf 'Flashing and running the test (timeout: %ss)...\n' "${TEST_TIMEOUT_SECONDS}"
set +e
timeout --foreground --signal=TERM --kill-after=5s "${TEST_TIMEOUT_SECONDS}s" \
    arm-none-eabi-gdb --batch --quiet "${FIRMWARE}" \
    -ex 'set pagination off' \
    -ex 'set confirm off' \
    -ex 'set remotetimeout 10' \
    -ex "target extended-remote :${GDB_PORT}" \
    -ex 'monitor reset halt' \
    -ex 'load' \
    -ex 'tbreak runtime_hardware_self_test_complete' \
    -ex 'continue' \
    -ex 'printf "SELFTEST_STATUS=0x%08x\n", (unsigned int)runtime_hardware_self_test_status' \
    -ex 'printf "SELFTEST_PHASE=%u\n", (unsigned int)runtime_hardware_self_test_phase' \
    -ex 'printf "FLASH_JEDEC_ID=0x%08x\n", (unsigned int)runtime_storage_diagnostics.flash_jedec_id' \
    -ex 'printf "FLASH_BLOCKS=%u\n", (unsigned int)runtime_storage_diagnostics.flash_blocks' \
    -ex 'printf "FLASH_LOGICAL_SECTORS=%u\n", (unsigned int)runtime_storage_diagnostics.flash_logical_sectors' \
    -ex 'printf "QSPI_ERROR=0x%08x\n", (unsigned int)runtime_storage_diagnostics.qspi_error' \
    -ex 'printf "LEVELX_STATUS=0x%08x\n", (unsigned int)runtime_storage_diagnostics.levelx_status' \
    -ex 'printf "FLASH_FILEX_STATUS=0x%08x\n", (unsigned int)runtime_storage_diagnostics.flash_filex_status' \
    -ex 'printf "FLASH_MOUNTED=%u\n", (unsigned int)runtime_storage_diagnostics.flash_mounted' \
    -ex 'printf "FLASH_WAS_FORMATTED=%u\n", (unsigned int)runtime_storage_diagnostics.flash_was_formatted' \
    -ex 'printf "SD_BLOCKS=%u\n", (unsigned int)runtime_storage_diagnostics.sd_blocks' \
    -ex 'printf "SD_ERROR=0x%08x\n", (unsigned int)runtime_storage_diagnostics.sd_error' \
    -ex 'printf "SD_FALLBACK_ERROR=0x%08x\n", (unsigned int)runtime_storage_diagnostics.sd_fallback_error' \
    -ex 'printf "SD_FILEX_STATUS=0x%08x\n", (unsigned int)runtime_storage_diagnostics.sd_filex_status' \
    -ex 'printf "SD_MOUNTED=%u\n", (unsigned int)runtime_storage_diagnostics.sd_mounted' \
    -ex 'printf "SD_DETECT_LEVEL=%u\n", (unsigned int)runtime_storage_diagnostics.sd_detect_level' \
    -ex 'printf "SD_BUS_WIDTH=%u\n", (unsigned int)runtime_storage_diagnostics.sd_bus_width' \
    -ex 'printf "SD_FALLBACK_USED=%u\n", (unsigned int)runtime_storage_diagnostics.sd_fallback_used' \
    -ex 'monitor resume' \
    -ex 'detach' \
    >"${GDB_LOG}" 2>&1
gdb_result=$?
set -e

status="$(result_value SELFTEST_STATUS)"
phase="$(result_value SELFTEST_PHASE)"

if [[ -z "${status}" || -z "${phase}" ]]; then
    printf '\nDebugger output:\n' >&2
    cat "${GDB_LOG}" >&2
    if ((gdb_result == 124)); then
        fail "the test did not finish within ${TEST_TIMEOUT_SECONDS}s"
    fi
    fail "the debugger did not return a complete test result (exit ${gdb_result})"
fi

printf '\nSTM32 hardware self-test report\n'
printf '  Result:                %s\n' "${status}"
printf '  Final phase:           %s/10 (%s)\n' "${phase}" "$(phase_name "${phase}")"
printf '  Flash JEDEC ID:        %s\n' "$(result_value FLASH_JEDEC_ID)"
printf '  Flash blocks:          %s\n' "$(result_value FLASH_BLOCKS)"
printf '  Flash logical sectors: %s\n' "$(result_value FLASH_LOGICAL_SECTORS)"
printf '  QSPI error:            %s\n' "$(result_value QSPI_ERROR)"
printf '  LevelX status:         %s\n' "$(result_value LEVELX_STATUS)"
printf '  Flash FileX status:    %s\n' "$(result_value FLASH_FILEX_STATUS)"
printf '  Flash mounted:         %s\n' "$(result_value FLASH_MOUNTED)"
printf '  Flash formatted now:   %s\n' "$(result_value FLASH_WAS_FORMATTED)"
printf '  SD blocks:             %s\n' "$(result_value SD_BLOCKS)"
printf '  SD error:              %s\n' "$(result_value SD_ERROR)"
printf '  SD fallback error:     %s\n' "$(result_value SD_FALLBACK_ERROR)"
printf '  SD FileX status:       %s\n' "$(result_value SD_FILEX_STATUS)"
printf '  SD mounted:            %s\n' "$(result_value SD_MOUNTED)"
printf '  SD detect level:       %s\n' "$(result_value SD_DETECT_LEVEL)"
printf '  SD bus width:          %s bit\n' "$(result_value SD_BUS_WIDTH)"
printf '  SD fallback used:      %s\n' "$(result_value SD_FALLBACK_USED)"

if [[ "${status,,}" == "0x600d600d" ]]; then
    printf '\nPASS: all ten hardware self-test phases completed successfully.\n'
    exit 0
fi

printf '\nFAIL: phase %s (%s) failed.\n' "${phase}" "$(phase_name "${phase}")" >&2
exit 1
