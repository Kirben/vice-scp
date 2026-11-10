"""Differential tests against the untouched upstream 1541 circuit.

The native oracle schedules individual upstream clocks and physical input
transitions independently, rather than duplicating the extracted circuit.
"""
import ctypes as C
import os
from pathlib import Path
import random
import sys
import time
import unittest

paths = [Path(sys.argv.pop(1)).resolve() for _ in range(2)]
if os.name == 'nt':
    dll_dirs = [os.add_dll_directory(str(p.parent)) for p in paths]
U64, U32, PTR = C.c_uint64, C.c_uint, C.c_void_p


class State(C.Structure):
    _fields_ = [('turn', U64), ('phase', U64), ('duration', U64), ('elapsed', U64),
                ('halftrack', U32), ('motor', U32), ('inclusive', U32)]


def api(lib, name, result, *args):
    f = getattr(lib, name)
    f.restype, f.argtypes = result, list(args)
    return f


class Circuit:
    def __init__(self, path, native=False):
        self.lib = C.CDLL(str(path))
        for name, result, args in [
            ('init', None, [C.c_uint32, U32, C.c_int]), ('finish', None, []),
            ('add_p64', None, [C.c_uint32, C.c_uint32]), ('p64_clocks', None, [U64]),
            ('cpu_clocks', None, [U64, U32]), ('pulse', None, []), ('ack', None, []),
            ('zone', None, [U32]), ('mode', None, [C.c_int]),
            ('state', U32, [C.POINTER(U64)])]:
            setattr(self, name, api(self.lib, 'electronics_' + name, result, *args))
        if native:
            for name, result, args in [
                ('bind', C.c_int, [PTR, U32]), ('advance', C.c_int, [U64]),
                ('unbind', None, []),
                ('drive_bind', None, []), ('drive_clock', C.c_int, [U64, U32]),
                ('drive_motor', None, [C.c_int]), ('drive_reset', None, []),
                ('wobble', None, [C.c_int, C.c_int]),
                ('step', C.c_int, [U32]), ('motor', None, [C.c_int]),
                ('flux_state', None, [C.POINTER(State), C.POINTER(U32)]),
                ('restore', C.c_int, [PTR, C.POINTER(State), U32]),
                ('invalid', C.c_int, [U32])]:
                setattr(self, name, api(self.lib, 'electronics_' + name, result, *args))

    def values(self):
        out = (U64 * 32)()
        assert self.state(out) == 32
        return list(out)

    def position(self):
        state, phase = State(), U32()
        self.flux_state(C.byref(state), C.byref(phase))
        return state, phase.value


old, new = Circuit(paths[0]), Circuit(paths[1], True)
create = api(new.lib, 'flux_capture_create', PTR, U32, U32)
add = api(new.lib, 'flux_capture_add_track', C.c_int, PTR, U32, C.POINTER(U64),
          C.POINTER(C.c_uint16), C.c_size_t, C.c_uint32)
seal = api(new.lib, 'flux_capture_seal', C.c_int, PTR)
destroy = api(new.lib, 'flux_capture_destroy', None, PTR)


def packed(pulses, unit):
    words, previous = [], 0
    for at in pulses:
        delta = (at - previous) // unit
        assert at > previous and (at - previous) % unit == 0
        # A zero is an overflow extension, never a pulse.
        while delta > 65535:
            words.append(0)
            delta -= 65536
        assert delta > 0, 'exact overflow-multiple edge needs another representation'
        words.append(delta)
        previous = at
    return words


class ClockOracle:
    def __init__(self, pulses, period):
        self.time = 0
        self.pulses, self.period = pulses, period

    def run(self, ticks):
        end = self.time + ticks
        events = [(t, 0) for t in range((self.time // 5 + 1) * 5, end + 1, 5)]
        if self.pulses:
            for turn in range(self.time // self.period, end // self.period + 1):
                events.extend((turn * self.period + p, 1) for p in self.pulses
                              if self.time < turn * self.period + p <= end)
        # Clock before flux when their timestamps are equal.
        for _, kind in sorted(events):
            old.pulse() if kind else old.p64_clocks(1)
        self.time = end


class ElectronicsTests(unittest.TestCase):
    def setUp(self):
        self.captures = []

    def tearDown(self):
        old.finish()
        new.finish()
        for capture in self.captures:
            destroy(capture)

    def capture(self, durations, pulses, unit=2, halftrack=0):
        capture = create(len(durations), 1)
        self.assertTrue(capture)
        self.captures.append(capture)
        self.add_track(capture, halftrack, durations, pulses, unit)
        self.assertEqual(seal(capture), 0)
        return capture

    def add_track(self, capture, halftrack, durations, pulses, unit=2):
        words = packed(pulses, unit)
        self.assertEqual(add(capture, halftrack, (U64 * len(durations))(*durations),
                             (C.c_uint16 * len(words))(*words), len(words), unit), 0)

    def same(self, native=False):
        a, b = old.values(), new.values()
        # Source positioning/storage differs; all electronics and output state
        # must agree. P64 comparisons include these fields as well.
        omitted = {19, 28, 29, 30} if native else set()
        for i, (x, y) in enumerate(zip(a, b)):
            if i not in omitted:
                self.assertEqual(x, y, f'circuit field {i}: upstream={a}, modified={b}')

    def test_p64_read_state_traces_unchanged(self):
        rng = random.Random(15541)
        for scenario in range(64):
            seed, zone = rng.getrandbits(32), scenario % 4
            old.init(seed, zone, 1)
            new.init(seed, zone, 1)
            for at in sorted(rng.sample(range(1, 100000), 180)):
                strength = rng.choice([0xffffffff, 0, 0x70000000, 0xa0000000])
                old.add_p64(at, strength)
                new.add_p64(at, strength)
            for _ in range(40):
                clocks = rng.randint(1, 1500)
                old.p64_clocks(clocks)
                new.p64_clocks(clocks)
                self.same()
                old.ack()
                new.ack()

    def test_p64_write_and_mode_switch_traces_unchanged(self):
        rng = random.Random(2718)
        for scenario in range(20):
            seed, zone = rng.getrandbits(32), scenario % 4
            old.init(seed, zone, 0)
            new.init(seed, zone, 0)
            for at in [0, 5, 99, 101, 2500, 10000]:
                old.add_p64(at, 0x90000000)
                new.add_p64(at, 0x90000000)
            for step in range(20):
                mode = (step // 3) % 2
                old.mode(mode)
                new.mode(mode)
                clocks = rng.randint(1, 2000)
                old.p64_clocks(clocks)
                new.p64_clocks(clocks)
                self.same()

    def test_p64_rotation_wrap_unchanged(self):
        for zone in range(4):
            old.init(9182, zone, 1)
            new.init(9182, zone, 1)
            for at in [0, 1, 39, 3199998, 3199999]:
                old.add_p64(at, 0xffffffff)
                new.add_p64(at, 0xffffffff)
            for clocks in [3199997, 1, 1, 1, 2, 39, 3200010]:
                old.p64_clocks(clocks)
                new.p64_clocks(clocks)
                self.same()

    def test_p64_cpu_catchup_and_bus_delay_unchanged(self):
        rng = random.Random(8080)
        for zone in range(4):
            old.init(771, zone, 1)
            new.init(771, zone, 1)
            for at in range(50, 16000, 57):
                old.add_p64(at, 0xffffffff)
                new.add_p64(at, 0xffffffff)
            for _ in range(100):
                cycles, requested = rng.randint(0, 100), rng.randint(0, 15)
                old.cpu_clocks(cycles, requested)
                new.cpu_clocks(cycles, requested)
                self.same()

    def test_native_matches_upstream_clock_oracle_random_partitions(self):
        rng = random.Random(53177)
        for scenario in range(60):
            durations = [rng.randint(600, 1500) * 2 for _ in range(1 + scenario % 3)]
            period = sum(durations)
            pulses = sorted(rng.sample(range(2, period, 2), min(130, period // 4)))
            capture = self.capture(durations, pulses)
            seed = rng.getrandbits(32)
            old.init(seed, scenario % 4, 1)
            new.init(seed, scenario % 4, 1)
            # Reach a real circuit state before comparing batched native
            # input with single clocks. Arbitrary injected registers can
            # describe an impossible pending filter transition at counter 40.
            old.p64_clocks(1000)
            new.p64_clocks(1000)
            self.same()
            self.assertEqual(new.bind(capture, 0), 0)
            oracle = ClockOracle(pulses, period)
            for _ in range(100):
                ticks = rng.randint(0, 220)
                oracle.run(ticks)
                self.assertEqual(new.advance(ticks), 0)
                self.same(native=True)
                state, phase = new.position()
                self.assertEqual((state.elapsed, phase), (oracle.time, oracle.time % 5))
                old.ack()
                new.ack()

    def test_native_clock_and_filter_boundary_ties(self):
        for pulses in [[198, 200, 202], [2, 4, 6, 8, 10], [40, 240, 440], [2, 200, 400, 600]]:
            capture = self.capture([800], pulses)
            old.init(0, 3, 1)
            new.init(0, 3, 1)
            self.assertEqual(new.bind(capture, 0), 0)
            oracle = ClockOracle(pulses, 800)
            for _ in range(1800):
                oracle.run(1)
                self.assertEqual(new.advance(1), 0)
                self.same(native=True)

    def test_native_overflow_gap_and_variable_turns(self):
        pulses = [10, 262158, 262400, 262402, 262404]
        durations = [140000, 140002]
        capture = self.capture(durations, pulses)
        old.init(981, 0, 1)
        new.init(981, 0, 1)
        self.assertEqual(new.bind(capture, 0), 0)
        oracle = ClockOracle(pulses, sum(durations))
        for ticks in [139999, 3, 122155, 1, 1, 2, 4, 50000, 400000]:
            oracle.run(ticks)
            self.assertEqual(new.advance(ticks), 0)
            self.same(native=True)

    def test_native_empty_turns_and_long_catchup(self):
        capture = self.capture([202, 198, 204], [])
        old.init(0, 0, 1)
        new.init(0, 0, 1)
        self.assertEqual(new.bind(capture, 0), 0)
        oracle = ClockOracle([], 604)
        oracle.run(60401)
        self.assertEqual(new.advance(60401), 0)
        self.same(native=True)
        state, phase = new.position()
        self.assertEqual((state.turn, state.phase, phase), (300, 1, 1))

    def test_native_edge_exactly_at_capture_end_is_retained(self):
        capture = self.capture([1000], [200, 600, 1000])
        old.init(0, 3, 1)
        new.init(0, 3, 1)
        self.assertEqual(new.bind(capture, 0), 0)
        oracle = ClockOracle([200, 600, 1000], 1000)
        for ticks in [999, 1, 0, 1, 999, 1000]:
            oracle.run(ticks)
            self.assertEqual(new.advance(ticks), 0)
            self.same(native=True)

    def test_native_motor_off_freezes_reference_and_capture(self):
        capture = self.capture([2000], [2, 10, 30, 300, 900])
        old.init(19, 2, 1)
        new.init(19, 2, 1)
        self.assertEqual(new.bind(capture, 0), 0)
        oracle = ClockOracle([2, 10, 30, 300, 900], 2000)
        oracle.run(33)
        self.assertEqual(new.advance(33), 0)
        self.same(native=True)
        before = new.values()
        state, phase = new.position()
        new.motor(0)
        self.assertEqual(new.advance(99999999), 0)
        paused, paused_phase = new.position()
        self.assertEqual(new.values(), before)
        self.assertEqual((paused.elapsed, paused.phase, paused_phase), (state.elapsed, state.phase, phase))
        new.motor(1)
        oracle.run(1003)
        self.assertEqual(new.advance(1003), 0)
        self.same(native=True)

    def test_native_gcr_sync_and_byte_ready_decode_known_bytes(self):
        payload = bytes.fromhex('5254a5294a965a6aa5a9')
        bits = [1] * 64 + [(byte >> bit) & 1 for byte in payload for bit in range(7, -1, -1)]
        for zone in range(4):
            bit_ticks = (16 - zone) * 4 * 5
            for shift in [0, 2, 4]:
                pulses = [(i + 1) * bit_ticks + shift for i, bit in enumerate(bits) if bit]
                capture = self.capture([16000000], pulses)
                new.init(0, zone, 1)
                self.assertEqual(new.bind(capture, 0), 0)
                decoded, synced = bytearray(), False
                for _ in range((len(bits) * bit_ticks + 1000) // 5):
                    self.assertEqual(new.advance(5), 0)
                    values = new.values()
                    synced |= values[2] == 1023
                    if values[24]:
                        if synced:
                            decoded.append(values[22])
                        new.ack()
                with self.subTest(zone=zone, shift=shift):
                    self.assertTrue(synced)
                    self.assertIn(payload, bytes(decoded))
    def test_native_head_step_keeps_reference_phase(self):
        capture = create(1, 1)
        self.captures.append(capture)
        self.add_track(capture, 0, [1000], [100, 600])
        self.add_track(capture, 1, [1500], [200, 900])
        self.assertEqual(seal(capture), 0)
        old.init(73, 0, 1)
        new.init(73, 0, 1)
        self.assertEqual(new.bind(capture, 0), 0)
        oracle = ClockOracle([100, 600], 1000)
        oracle.run(403)
        self.assertEqual(new.advance(403), 0)
        self.same(native=True)
        self.assertEqual(new.step(1), 0)
        state, phase = new.position()
        self.assertEqual((state.phase, state.duration, phase), (604, 1500, 3))
        # Next destination input is at phase 900, in 296 native ticks.
        for at in range(1, 400):
            if (403 + at) % 5 == 0:
                old.p64_clocks(1)
            if at == 296:
                old.pulse()
            self.assertEqual(new.advance(1), 0)
            self.same(native=True)

    def test_native_missing_track_keeps_clock_running(self):
        capture = self.capture([1000], [100])
        old.init(33, 2, 1)
        new.init(33, 2, 1)
        self.assertEqual(new.bind(capture, 2), 0)
        self.assertEqual(new.advance(500), 0)
        old.p64_clocks(100)
        self.same(native=True)

    def test_native_write_gate_does_not_mutate_or_stop_capture(self):
        capture = self.capture([2000], [20, 400, 800, 1200])
        new.init(0, 2, 0)
        self.assertEqual(new.bind(capture, 0), 0)
        self.assertEqual(new.advance(1001), 0)
        state, phase = new.position()
        self.assertEqual((state.elapsed, state.phase, phase), (1001, 1001, 1))
        self.assertEqual(new.values()[28], 0, 'native input called a P64 writer')
        self.assertEqual(new.values()[31], 0, 'native input marked P64 dirty')

    def test_native_invalid_adapter_arguments(self):
        capture = self.capture([1000], [100])
        new.init(0, 0, 1)
        self.assertEqual(new.bind(capture, 0), 0)
        before = new.values()
        for which in range(5):
            self.assertEqual(new.invalid(which), -1)
        self.assertEqual(new.values(), before)

    def test_native_unbind_and_failed_rebind_preserve_electronics(self):
        capture = self.capture([1000], [100])
        new.init(0, 0, 1)
        self.assertEqual(new.bind(capture, 0), 0)
        self.assertEqual(new.advance(7), 0)
        before = new.values()
        state, phase = new.position()
        self.assertEqual(new.bind(None, 0), -1)
        retained, retained_phase = new.position()
        self.assertEqual((retained.elapsed, retained_phase), (state.elapsed, phase))
        new.unbind()
        new.unbind()
        self.assertEqual(new.advance(5), -1)
        self.assertEqual(new.values(), before)
        self.assertEqual(new.bind(capture, 0), 0)
        self.assertEqual(new.values(), before)

    def test_native_elapsed_range_error_does_not_advance(self):
        capture = self.capture([1000], [100])
        new.init(0, 0, 1)
        self.assertEqual(new.bind(capture, 0), 0)
        self.assertEqual(new.advance(1), 0)
        before = new.values()
        state, phase = new.position()
        self.assertEqual(new.advance((1 << 64) - 1), -5)
        after, after_phase = new.position()
        self.assertEqual(new.values(), before)
        self.assertEqual((after.elapsed, after_phase), (state.elapsed, phase))

    def test_native_turn_overflow_stops_at_last_completed_span(self):
        capture = self.capture([2], [])
        new.init(0, 0, 1)
        self.assertEqual(new.bind(capture, 0), 0)
        state, _ = new.position()
        state.turn = (1 << 64) - 1
        self.assertEqual(new.restore(capture, C.byref(state), 4), 0)
        self.assertEqual(new.advance(2), -5)
        after, phase = new.position()
        # The first clock at +1 succeeds; the following turn boundary at +2
        # would overflow and must not be consumed or update the electronics.
        self.assertEqual((after.elapsed, after.turn, after.phase, phase),
                         (1, (1 << 64) - 1, 1, 0))
        self.assertEqual(new.values()[17], 1)

    def test_native_one_second_smoke_performance(self):
        capture = self.capture([16000000], range(240, 16000000, 240))
        new.init(0, 3, 1)
        self.assertEqual(new.bind(capture, 0), 0)
        start = time.perf_counter()
        self.assertEqual(new.advance(80000000), 0)
        elapsed = time.perf_counter() - start
        state, phase = new.position()
        self.assertEqual((state.turn, state.phase, state.elapsed, phase), (5, 0, 80000000, 0))
        print(f'Native one-second circuit smoke check: {elapsed:.3f}s wall time')
        self.assertLess(elapsed, 30, 'unbounded or unexpectedly slow catch-up')

    def test_drive_cpu_dispatch_and_bus_delays_match_upstream(self):
        capture = self.capture([16000000], [])
        old.init(0, 2, 1)
        new.init(0, 2, 1)
        self.assertEqual(new.bind(capture, 0), 0)
        new.drive_bind()
        rng = random.Random(85141)
        cpu, target = 0, 0
        for _ in range(300):
            clocks, request = rng.randrange(100), rng.randrange(16)
            cpu += clocks
            target = max(target, cpu * 16 + request)
            old.cpu_clocks(clocks, request)
            self.assertEqual(new.drive_clock(clocks, request), 0)
            self.same(native=True)
            state, phase = new.position()
            self.assertEqual((state.elapsed, phase), (target * 5, 0))

    def test_drive_long_cpu_catchup_keeps_every_turn_and_ignores_wobble(self):
        capture = self.capture([16000000], [])
        new.init(0, 0, 1)
        self.assertEqual(new.bind(capture, 0), 0)
        new.drive_bind()
        new.wobble(28000, 5000)
        self.assertEqual(new.drive_clock(1000000, 0), 0)
        state, phase = new.position()
        self.assertEqual((state.elapsed, state.turn, state.phase, phase), (80000000, 5, 0, 0))

    def test_drive_motor_edges_do_not_catch_up_stopped_time(self):
        capture = self.capture([16000000], [100, 1000, 2000])
        new.init(0, 2, 1)
        self.assertEqual(new.bind(capture, 0), 0)
        new.drive_bind()
        self.assertEqual(new.drive_clock(10, 0), 0)
        before, phase = new.position()
        new.drive_motor(0)
        self.assertEqual(new.drive_clock(1000000, 15), 0)
        stopped, _ = new.position()
        self.assertEqual(stopped.elapsed, before.elapsed)
        new.drive_motor(1)
        self.assertEqual(new.drive_clock(10, 0), 0)
        resumed, _ = new.position()
        self.assertEqual(resumed.elapsed, before.elapsed + 800)

    def test_drive_reset_preserves_mechanical_phase(self):
        capture = self.capture([16000000], [100, 1000, 2000])
        new.init(0, 2, 1)
        self.assertEqual(new.bind(capture, 0), 0)
        new.drive_bind()
        self.assertEqual(new.drive_clock(20, 3), 0)
        before, _ = new.position()
        new.drive_reset()
        after, phase = new.position()
        self.assertEqual((after.turn, after.phase, after.elapsed), (before.turn, before.phase, before.elapsed))
        self.assertEqual(phase, 0)
        self.assertEqual(new.drive_clock(1, 0), 0)
        after, _ = new.position()
        self.assertEqual(after.elapsed, before.elapsed + 80)


if __name__ == '__main__':
    unittest.main(verbosity=2)
