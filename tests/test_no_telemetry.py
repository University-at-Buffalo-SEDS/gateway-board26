"""SDK-free mode keeps local acquisition and its watchdog operational."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import build

ROOT = Path(__file__).resolve().parents[1]


class NoTelemetryTests(unittest.TestCase):
    def test_build_and_flash_forward_disabled_telemetry(self):
        for command in ('build', 'flash'):
            args = build.make_parser().parse_args(
                [command, '--release', '--no-telemetry', '--allocator', 'tlsf'])
            config = build.build_cfg_from_args(mock.Mock(), args)
            self.assertFalse(config.telemetry)
            with mock.patch.object(build, 'run') as execute, mock.patch.object(
                    build, 'pick_elf', return_value=Path('/tmp/test.elf')):
                build.configure_and_build(mock.Mock(), config)
            self.assertIn('-DENABLE_TELEMETRY=OFF', execute.call_args_list[0].args[1])

    def test_local_headers_do_not_import_the_sdk(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'sedsnet.h').write_text('#error SDK must not be included\n')
            subprocess.run(
                ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', directory,
                 '-I', str(ROOT / 'Core/Inc'), '-x', 'c', '-', '-fsyntax-only'],
                input='#include "flight_state_cache.h"\n#include "board_watchdog.h"\n'
                      '_Static_assert(BOARD_WATCHDOG_REQUIRED_MASK == BOARD_WATCHDOG_ACQUISITION, '
                      '"SDK-free mode monitors acquisition");\n',
                text=True, check=True)

    def test_acquisition_can_feed_watchdog_without_network_task(self):
        source = (ROOT / 'Core/Src/board_watchdog.c').read_text()
        source = '\n'.join(line for line in source.splitlines()
                           if not line.startswith('#include'))
        code = r'''
#include <assert.h>
#include <stdint.h>
#include "board_watchdog.h"
#include "board_uptime.h"
#define BOARD_WATCHDOG_ENABLE 1
#define IWDG_SR_PVU 1U
#define IWDG_SR_RVU 2U
#define IWDG_SR_WVU 4U
#define TX_TIMER_TICKS_PER_SECOND 1000U
#define __HAL_RCC_CLEAR_RESET_FLAGS() ((void)0)
typedef uint32_t ULONG;
static struct { uint32_t CSR; } rcc;
#define RCC (&rcc)
static struct { uint32_t KR, PR, RLR, SR; } iwdg;
#define IWDG (&iwdg)
static uint32_t tick;
static ULONG tx_time_get(void) { return tick; }
static uint32_t HAL_GetTick(void) { return tick; }
static uint32_t __get_PRIMASK(void) { return 0; }
static void __disable_irq(void) {}
static void __set_PRIMASK(uint32_t value) { (void)value; }
static void __NOP(void) {}
''' + source + r'''
int main(void) {
    board_watchdog_start();
    assert(g_watchdog_started == 1U);
    tick = 250;
    board_watchdog_progress(BOARD_WATCHDOG_NETWORK);
    assert(g_watchdog_feed_count == 0U);
    board_watchdog_progress(BOARD_WATCHDOG_ACQUISITION);
    assert(g_watchdog_feed_count == 1U);
    tick += 250;
    board_watchdog_progress(BOARD_WATCHDOG_NETWORK);
    assert(g_watchdog_feed_count == 1U);
    board_watchdog_progress(BOARD_WATCHDOG_ACQUISITION);
    assert(g_watchdog_feed_count == 2U);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'watchdog'
            subprocess.run(
                ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I',
                 str(ROOT / 'Core/Inc'), '-x', 'c', '-', '-o', str(binary)],
                input=code, text=True, check=True)
            subprocess.run([str(binary)], check=True, timeout=5)
