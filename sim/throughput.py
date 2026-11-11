"""Single-board Gateway throughput scenario (FirmwareSimulator).

Builds a measurement image (simulator probes + DWT cycle profile + synthetic
CAN ingress at --fps), runs it alone in Renode, and reports the sustained
CAN->UART forwarding rate, every loss counter and the per-path profile.

    python3 sim/throughput.py --fps 600            # pass/fail at the target
    python3 sim/throughput.py --sweep 200,400,800  # saturation search

Limits, stated in every report: Renode is not cycle accurate. It executes a
fixed 100 M instructions per virtual second while DWT CYCCNT advances at the
170 MHz core clock, so one profiled "cycle" here is 1/1.7 of an instruction.
Rates and profile shares are relative evidence; real cycles come from the
bench procedure in docs/throughput.md. The UART peripheral model does time
each character at the configured baud, so the 1 Mbaud link limit is real.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "sim"))
sys.path.insert(0, str(ROOT))

# Twice the measured fill-bus offered load (291 frames/s at the gateway CAN
# controller; 166/s of it is forwarded to GroundStation), rounded up. Every
# synthetic frame is forwarded, so this also exceeds 3.6x today's forwarded rate.
TARGET_FPS = 600
# Measurement images never share a directory with flashable release builds.
BUILD_SUBDIR = "Throughput_Script"
MIN_DELIVERED = 0.99

SLOTS = ["uart_service", "can_drain", "queues", "discovery", "timesync", "ota",
         "wait", "can_isr", "uart_isr", "can_ingress", "uart_ingress", "uart_tx",
         "can_tx", "alloc", "free", "admission", "loadgen"]
# Top-level CPU users: thread phases other than the wait, plus both ISRs.
# Nested slots are already inside their parents.
TOP_LEVEL = ["uart_service", "can_drain", "queues", "discovery", "timesync",
             "ota", "can_isr", "uart_isr", "loadgen"]
COUNTERS = {
    "injected": "g_gateway_loadgen_injected",
    "loadgen_state": "g_gateway_loadgen_state",
    "uart_tx_frames": "g_gateway_uart_tx_frames",
    "uart_tx_queue_drops": "g_gateway_uart_tx_queue_drops",
    "uart_tx_high_water": "g_gateway_uart_tx_high_water",
    "can_ring_drops": "g_rx_dropped_frames",
    "can_handoff_fail": "g_gateway_can_rx_handoff_fail",
    "can_admission_fail": "g_gateway_can_admission_fail",
    "admission_drops": "g_gateway_memory_admission_drops",
    "alloc_fail": "g_telemetry_alloc_fail",
    "loops": "g_gateway_telemetry_loop_count",
    "wall": "g_gateway_cycle_profile_wall",
    "fdcan_tx_ok": "g_fdcan_tx_ok_count",
    "can_tx_pending": "g_can_tx_pending",
    "can_tx_rejected": "g_can_tx_rejected",
    "can_recoveries": "g_fdcan_recovery_attempt_count",
}
# SEDSnet seds_router_copy_queue_counters() words, mirrored by the sim build
# into g_telemetry_alloc_failure_stack[20..35] once per second.
QUEUE_WORDS = {"q_rx_len": 1, "q_tx_len": 2, "q_tx_bytes": 4, "q_front_type": 5,
               "q_front_retry_ms": 7, "q_front_kind": 13}


def probes() -> list[dict]:
    result = [{"name": name, "symbol": symbol} for name, symbol in COUNTERS.items()]
    result += [{"name": name, "symbol": "g_telemetry_alloc_failure_stack",
                "byte_offset": 4 * (20 + word)} for name, word in QUEUE_WORDS.items()]
    for index, slot in enumerate(SLOTS):
        for word, field in enumerate(("count", "max", "total")):
            result.append({"name": f"p_{slot}_{field}", "symbol": "g_gateway_cycle_profile",
                           "byte_offset": 12 * index + 4 * word})
    result.append({"name": "telemetry_stack_remaining",
                   "symbol": "g_telemetry_stack_remaining", "minimum": 1})
    return result


def build(fps: int) -> None:
    env = dict(os.environ, SEDS_FIRMWARE_SIM_TEST="1", GATEWAY_CYCLE_PROFILE="1",
               GATEWAY_LOADGEN_FPS=str(fps))
    env.setdefault("CMAKE_BUILD_PARALLEL_LEVEL", "4")
    env.setdefault("CARGO_BUILD_JOBS", "4")
    for image in ("factory", "ota"):  # the simulator validates both artifacts
        subprocess.run([sys.executable, str(ROOT / "build.py"), "--build-subdir",
                        BUILD_SUBDIR, "build", "--release", "--image", image],
                       cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL)


def simulate(virtual_ms: int, samples: int, attempts: int = 2) -> dict[str, list[int]]:
    """Run once; retry a simulator-side failure (e.g. a missed probe sample
    on a heavily loaded host). Virtual time is deterministic, so a retry
    measures the same firmware behaviour."""
    for attempt in range(attempts):
        try:
            return simulate_once(virtual_ms, samples)
        except RuntimeError:
            if attempt + 1 == attempts:
                raise
    raise AssertionError("unreachable")


def simulate_once(virtual_ms: int, samples: int) -> dict[str, list[int]]:
    import run_full
    docker = run_full.require_docker()

    class Ui:
        def say(self, _kind, message):
            print(f"[SIM] {message}", file=sys.stderr, flush=True)
    image = run_full.resolve_simulator_image(Ui(), docker, ROOT, "stm32g4")
    layout = run_full.load_layout_for_build(ROOT, BUILD_SUBDIR)
    execution = layout["execution"]
    execution.update(virtual_time_ms=virtual_ms, sample_count=samples,
                     memory_probe_warmup_samples=0, trace=False)
    execution["memory_probes"] = probes()
    with tempfile.TemporaryDirectory(prefix="gb-throughput-") as directory:
        run_full.write_container_layout(Path(directory), layout)
        command = [*run_full.docker_run_prefix(docker),
                   "-v", f"{ROOT}:/firmware:ro", "-v", f"{directory}:/simulation:ro",
                   image, "--json", "run", "--layout", "/simulation/board.json",
                   "--firmware-root", "/firmware"]
        completed = subprocess.run(command, capture_output=True, text=True)
        if completed.returncode != 0:
            sys.stderr.write(completed.stdout[-4000:] + completed.stderr[-4000:])
            raise RuntimeError("simulator run failed")
        output = completed.stdout
    report = json.loads(output[output.index("{"):])
    return {probe["name"]: [int(v) for v in probe["samples"]]
            for probe in report["execution"]["memory_profile"]}


def unwrap(values: list[int]) -> list[int]:
    out, offset = [], 0
    for i, value in enumerate(values):
        if i and value < values[i - 1]:
            offset += 1 << 32
        out.append(value + offset)
    return out


def analyse(series: dict[str, list[int]], fps: int, sample_ms: float) -> dict:
    injected = series["injected"]
    start = next((i for i, v in enumerate(injected) if v > 0), None)
    if series["loadgen_state"][-1] != 1 or start is None or start + 3 > len(injected):
        raise RuntimeError(f"load generator did not run: state={series['loadgen_state']}")
    first, last = start + 1, len(injected) - 1  # skip the ramp-in interval
    seconds = (last - first) * sample_ms / 1000.0
    delta = lambda name: series[name][last] - series[name][first]
    wall = unwrap(series["wall"])
    wall_cycles = wall[last] - wall[first]
    profile = {}
    for slot in SLOTS:
        total = unwrap(series[f"p_{slot}_total"])
        count = series[f"p_{slot}_count"]
        cycles = total[last] - total[first]
        calls = count[last] - count[first]
        profile[slot] = {
            "share": cycles / wall_cycles if wall_cycles else 0.0,
            "calls_per_s": calls / seconds,
            "cycles_per_call": cycles / calls if calls else 0.0,
            "max_cycles": series[f"p_{slot}_max"][last],
        }
    busy = sum(profile[s]["share"] for s in TOP_LEVEL)
    result = {
        "offered_fps": fps,
        "window_s": seconds,
        "injected_fps": delta("injected") / seconds,
        "forwarded_fps": delta("uart_tx_frames") / seconds,
        "can_ring_drops": delta("can_ring_drops"),
        "can_handoff_fail": delta("can_handoff_fail"),
        "can_admission_fail": delta("can_admission_fail"),
        "admission_drops": delta("admission_drops"),
        "uart_tx_queue_drops": delta("uart_tx_queue_drops"),
        "alloc_fail": delta("alloc_fail"),
        "uart_tx_high_water": series["uart_tx_high_water"][last],
        "cpu_busy_share": busy,
        "profile": profile,
        "end_state": {name: series[name][last] for name in
                      [*QUEUE_WORDS, "fdcan_tx_ok", "can_tx_pending", "can_tx_rejected",
                       "can_recoveries"]},
    }
    result["delivered_ratio"] = (result["forwarded_fps"] / result["injected_fps"]
                                 if result["injected_fps"] else 0.0)
    result["pass"] = (result["delivered_ratio"] >= MIN_DELIVERED
                      and result["can_ring_drops"] == 0 and result["alloc_fail"] == 0)
    return result


def run(fps: int, virtual_ms: int, samples: int, skip_build: bool) -> dict:
    if not skip_build:
        build(fps)
    series = simulate(virtual_ms, samples)
    return analyse(series, fps, virtual_ms / samples)


def qualify_target(ui=None) -> None:
    """build.py test --all stage: forward TARGET_FPS with no loss."""
    result = run(TARGET_FPS, 10000, 10, False)
    summary = (f"forwarded {result['forwarded_fps']:.0f} of {result['injected_fps']:.0f} "
               f"frames/s, ring drops {result['can_ring_drops']}, allocation failures "
               f"{result['alloc_fail']}, relative CPU busy {100 * result['cpu_busy_share']:.0f}%")
    if ui is not None:
        ui.say("info", "Throughput: " + summary)
    if not result["pass"]:
        raise RuntimeError(f"Gateway missed the {TARGET_FPS} frames/s target: {summary}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fps", type=int, default=TARGET_FPS)
    parser.add_argument("--sweep", help="comma-separated offered rates; report only")
    parser.add_argument("--virtual-ms", type=int, default=10000)
    parser.add_argument("--samples", type=int, default=10)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    rates = [int(x) for x in args.sweep.split(",")] if args.sweep else [args.fps]
    results = []
    for fps in rates:
        result = run(fps, args.virtual_ms, args.samples, args.skip_build and len(rates) == 1)
        results.append(result)
        print(json.dumps(result, indent=2) if args.json else
              f"offered {fps}/s: injected {result['injected_fps']:.0f}/s, forwarded "
              f"{result['forwarded_fps']:.0f}/s ({100 * result['delivered_ratio']:.1f}%), "
              f"ring drops {result['can_ring_drops']}, handoff fails "
              f"{result['can_handoff_fail']}, UART queue refusals "
              f"{result['uart_tx_queue_drops']}, CPU busy {100 * result['cpu_busy_share']:.0f}% "
              f"(relative; Renode is not cycle accurate)", flush=True)
    if args.sweep:
        return 0
    return 0 if results[0]["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
