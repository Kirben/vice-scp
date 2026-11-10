"""Independent fixtures/oracles for the production native SCP core."""
import ctypes as C
import hashlib
import os
from pathlib import Path
import random
import struct
import sys
import unittest

LIBRARY = Path(sys.argv.pop(1)).resolve()
OUT = Path(sys.argv.pop(1)).resolve()
if os.name == 'nt':
    DLL_DIR = os.add_dll_directory(str(LIBRARY.parent))
lib = C.CDLL(str(LIBRARY))
U64 = C.c_uint64
U32 = C.c_uint
PTR = C.c_void_p
MAX = (1 << 64) - 1


class State(C.Structure):
    _fields_ = [('turn', U64), ('phase', U64), ('duration', U64), ('elapsed', U64),
                ('halftrack', U32), ('motor', U32), ('inclusive', U32)]


class Scan(C.Structure):
    _fields_ = [('word', C.c_size_t), ('time', U64)]


class Cursor(C.Structure):
    _fields_ = [('capture', PTR), ('track', PTR), ('state', State), ('scan', Scan),
                ('pulse', U64), ('has_pulse', C.c_int)]


class Event(C.Structure):
    _fields_ = [('delta', U64), ('kind', U32)]


def api(name, result, *args):
    fn = getattr(lib, name)
    fn.restype = result
    fn.argtypes = list(args)
    return fn


create = api('flux_capture_create', PTR, U32, U32)
add = api('flux_capture_add_track', C.c_int, PTR, U32, C.POINTER(U64), C.POINTER(C.c_uint16), C.c_size_t, C.c_uint32)
seal = api('flux_capture_seal', C.c_int, PTR)
destroy = api('flux_capture_destroy', None, PTR)
track = api('flux_capture_track', PTR, PTR, U32)
is_sealed = api('flux_capture_is_sealed', C.c_int, PTR)
revs = api('flux_capture_revolutions', U32, PTR)
flags = api('flux_capture_flags', U32, PTR)
word_count = api('flux_capture_words', C.c_size_t, PTR)
bind = api('flux_playback_bind', C.c_int, C.POINTER(Cursor), PTR, U32)
next_event = api('flux_playback_next_event', C.c_int, C.POINTER(Cursor), C.POINTER(Event))
advance = api('flux_playback_advance', C.c_int, C.POINTER(Cursor), U64, C.POINTER(U32))
step = api('flux_playback_change_track', C.c_int, C.POINTER(Cursor), U32)
motor = api('flux_playback_set_motor', None, C.POINTER(Cursor), C.c_int)
restore = api('flux_playback_restore', C.c_int, C.POINTER(Cursor), PTR, C.POINTER(State))
read = api('native_test_read', C.c_int, C.c_char_p, C.POINTER(PTR))
live = api('native_test_live', C.c_int)
reset = api('native_test_reset', None)
fail_alloc = api('native_test_fail_alloc', None, C.c_int)
fail_read = api('native_test_fail_read', None, C.c_int)
fail_seek = api('native_test_fail_seek', None, C.c_int)
fake_size = api('native_test_file_size', None, C.c_long)
seek = api('flux_track_seek', C.c_int, PTR, U64, C.c_int, C.POINTER(Scan), C.POINTER(U64))


def checksum(data):
    data = bytearray(data)
    struct.pack_into('<I', data, 12, 0 if data[8] & 16 else sum(data[16:]) & 0xffffffff)
    return data


def scp(tracks=None, *, flags=1, resolution=0, heads=1, width=0,
        disk_type=0, start=0, end=None, entries=168, revolutions=1):
    if tracks is None:
        tracks = {0: [(8000000, [80, 160, 240])]}
    if tracks:
        revolutions = len(next(iter(tracks.values())))
    if end is None:
        end = max(tracks) if tracks else 83
    data = bytearray(16 + entries * 4)
    data[:16] = struct.pack('<3s9BI', b'SCP', 0x17, disk_type, revolutions,
                           start, end, flags, width, heads, resolution, 0)
    for halftrack, turns in tracks.items():
        struct.pack_into('<I', data, 16 + halftrack * 4, len(data))
        tdh = bytearray(b'TRK' + bytes([halftrack]))
        raw = bytearray()
        for duration, words in turns:
            tdh += struct.pack('<III', duration, len(words), 4 + revolutions * 12 + len(raw))
            raw += struct.pack('>' + 'H' * len(words), *words)
        data += tdh + raw
    return checksum(data)


def metadata(flags=0x21, revision=0x16):
    data = scp(flags=flags & ~0x20)
    footer = bytearray(48)
    for i, text in enumerate(['Drive', 'Model', '', 'Application', 'Comment', '\u30c6\u30b9\u30c8']):
        raw = text.encode('utf-8')
        struct.pack_into('<I', footer, i * 4, len(data))
        data += struct.pack('<H', len(raw)) + raw + b'\0'
    footer[43] = revision
    footer[44:] = b'FPCS'
    data += footer
    data[8] |= 32
    data[3] = 0
    return checksum(data)


class NativeTests(unittest.TestCase):
    def setUp(self):
        reset()
        self.captures = []
        self.assertEqual(live(), 0)

    def tearDown(self):
        reset()
        for capture in self.captures:
            destroy(capture)
        self.assertEqual(live(), 0, 'production allocation leaked')

    def load(self, data, expected=0, name=None):
        path = OUT / (name or self.id().split('.')[-1] + '.scp')
        path.write_bytes(data)
        before = hashlib.sha256(data).digest()
        capture = PTR()
        rc = read(str(path).encode('utf-8'), C.byref(capture))
        self.assertEqual(rc, expected)
        self.assertEqual(hashlib.sha256(path.read_bytes()).digest(), before)
        self.assertEqual(bool(capture), expected == 0)
        if capture:
            self.captures.append(capture)
        return capture

    def build_capture(self, tracks, indexed=1, sealed=True):
        count = len(next(iter(tracks.values()))[0])
        capture = create(count, indexed)
        self.assertTrue(capture)
        self.captures.append(capture)
        for halftrack, (durations, words, unit) in tracks.items():
            self.assertEqual(add(capture, halftrack, (U64 * count)(*durations),
                                 (C.c_uint16 * len(words))(*words), len(words), unit), 0)
        if sealed:
            self.assertEqual(seal(capture), 0)
        return capture

    def cursor(self, capture, halftrack=0):
        value = Cursor()
        self.assertEqual(bind(C.byref(value), capture, halftrack), 0)
        return value

    def run_ticks(self, cursor, ticks, seed=None):
        randomizer = random.Random(seed) if seed is not None else None
        trace = []
        remaining = ticks
        for _ in range(200000):
            event = Event()
            self.assertEqual(next_event(C.byref(cursor), C.byref(event)), 0)
            if not event.kind or (remaining == 0 and event.delta):
                return trace
            amount = min(event.delta, remaining)
            if randomizer and amount:
                amount = min(amount, randomizer.randrange(1, 200))
            kind = U32()
            self.assertEqual(advance(C.byref(cursor), amount, C.byref(kind)), 0)
            remaining -= amount
            if kind.value:
                trace.append((cursor.state.elapsed, kind.value))
            if not remaining:
                following = Event()
                next_event(C.byref(cursor), C.byref(following))
                if following.delta:
                    return trace
        self.fail('zero-time event loop or too many events')

    def test_precise_timestamps_are_not_p64_positions(self):
        cursor = self.cursor(self.load(scp({0: [(1000, [1, 2, 3])]})))
        self.assertEqual(self.run_ticks(cursor, 2000), [(2, 1), (6, 1), (12, 1), (2000, 6)])

    def test_distinct_revolutions_and_original_durations(self):
        cap = self.load(scp({0: [(1000, [100, 100]), (2000, [1000]), (500, [1900])]}))
        cursor = self.cursor(cap)
        self.assertEqual(self.run_ticks(cursor, 7000),
                         [(200, 1), (400, 1), (2000, 2), (2400, 1),
                          (6000, 2), (6200, 1), (7000, 6)])

    def test_resolution_multiplier_does_not_scale_index_duration(self):
        cap = self.load(scp({0: [(1000, [10, 20])]}, resolution=2))
        self.assertEqual(self.run_ticks(self.cursor(cap), 2000), [(60, 1), (180, 1), (2000, 6)])

    def test_overflow_is_not_a_transition(self):
        cap = self.load(scp({0: [(1000000, [0, 0, 17])]}))
        self.assertEqual(self.run_ticks(self.cursor(cap), 300000), [(262178, 1)])

    def test_interval_carries_between_revolution_records(self):
        cap = self.load(scp({0: [(1000, [900]), (1000, [300])]}))
        self.assertEqual(self.run_ticks(self.cursor(cap), 4000), [(1800, 1), (2000, 2), (2400, 1), (4000, 6)])

    def test_overflow_carries_between_records(self):
        cap = self.load(scp({0: [(70000, [0]), (70000, [0, 100])]}))
        self.assertEqual(self.run_ticks(self.cursor(cap), 280000), [(140000, 2), (262344, 1), (280000, 6)])

    def test_exact_index_edge_occurs_after_boundary(self):
        cap = self.load(scp({0: [(1000, [1000]), (1000, [100])]}))
        self.assertEqual(self.run_ticks(self.cursor(cap), 2400), [(2000, 2), (2000, 1), (2200, 1)])

    def test_no_phantom_edge_at_capture_loop(self):
        cap = self.load(scp({0: [(1000, [100, 500])]}))
        self.assertEqual(self.run_ticks(self.cursor(cap), 2300), [(200, 1), (1200, 1), (2000, 6), (2200, 1)])

    def test_real_capture_endpoint_edge_replayed_once(self):
        cap = self.load(scp({0: [(1000, [100, 900])]}))
        cursor = self.cursor(cap)
        self.assertEqual(self.run_ticks(cursor, 4000),
                         [(200, 1), (2000, 6), (2000, 1), (2200, 1), (4000, 6), (4000, 1)])
        self.assertEqual(self.run_ticks(cursor, 0), [])

    def test_capture_endpoint_edge_restore_before_and_after(self):
        cap = self.load(scp({0: [(1000, [100, 900])]}))
        cursor = self.cursor(cap)
        self.run_ticks(cursor, 1999)
        kind = U32()
        self.assertEqual(advance(C.byref(cursor), 1, C.byref(kind)), 0)
        self.assertEqual(kind.value, 6)
        saved = State.from_buffer_copy(bytes(cursor.state))
        restored = Cursor()
        self.assertEqual(restore(C.byref(restored), cap, C.byref(saved)), 0)
        self.assertEqual(self.run_ticks(restored, 0), [(2000, 1)])
        consumed = State.from_buffer_copy(bytes(restored.state))
        self.assertEqual(restore(C.byref(restored), cap, C.byref(consumed)), 0)
        self.assertEqual(self.run_ticks(restored, 0), [])
        self.assertEqual(self.run_ticks(restored, 200), [(2200, 1)])

    def test_uncued_first_window_is_not_discarded(self):
        cap = self.load(scp({0: [(1000, [100]), (1000, [1100])]}, flags=0))
        self.assertEqual(flags(cap), 0)
        self.assertEqual(self.run_ticks(self.cursor(cap), 4000), [(200, 1), (2000, 2), (2400, 1), (4000, 6)])

    def test_empty_revolution_is_preserved(self):
        cap = self.load(scp({0: [(1000, []), (1000, [1100])]}))
        cursor = self.cursor(cap)
        self.assertEqual(self.run_ticks(cursor, 6000), [(2000, 2), (2200, 1), (4000, 6), (6000, 2)])
        event = Event();next_event(C.byref(cursor), C.byref(event))
        self.assertEqual((event.delta, event.kind), (200, 1))

    def test_trailing_overflow_is_not_a_pulse(self):
        cap = self.load(scp({0: [(1000000, [10, 0, 0])]}))
        self.assertEqual(self.run_ticks(self.cursor(cap), 1000000), [(20, 1)])

    def test_samples_beyond_final_index_are_not_wrapped(self):
        cap = self.load(scp({0: [(100, [99, 2, 50])]}))
        self.assertEqual(self.run_ticks(self.cursor(cap), 200), [(198, 1), (200, 6)])

    def test_motor_stop_preserves_every_cursor_field(self):
        cap = self.load(scp())
        cursor = self.cursor(cap)
        self.run_ticks(cursor, 31)
        motor(C.byref(cursor), 0)
        saved = bytes(cursor)
        kind = U32(99)
        self.assertEqual(advance(C.byref(cursor), MAX, C.byref(kind)), 0)
        self.assertEqual(kind.value, 0)
        self.assertEqual(bytes(cursor), saved)
        motor(C.byref(cursor), 1)
        self.assertEqual(self.run_ticks(cursor, 129), [(160, 1)])

    def test_seek_preserves_angular_phase_with_different_durations(self):
        cap = self.build_capture({0: ([1000], [100], 1), 1: ([3000], [2100], 1)})
        cursor = self.cursor(cap)
        self.run_ticks(cursor, 600)
        self.assertEqual(step(C.byref(cursor), 1), 0)
        self.assertEqual((cursor.state.phase, cursor.state.duration, cursor.state.turn), (1800, 3000, 0))
        self.assertEqual(self.run_ticks(cursor, 300), [(900, 1)])

    def test_head_step_does_not_reset_revolution(self):
        cap = self.build_capture({0: ([1000, 2000], [100], 1), 1: ([3000, 4000], [3500], 1)})
        cursor = self.cursor(cap)
        self.run_ticks(cursor, 1400)
        step(C.byref(cursor), 1)
        self.assertEqual((cursor.state.turn, cursor.state.phase, cursor.state.duration), (1, 800, 4000))

    def test_missing_track_keeps_disk_turning(self):
        cap = self.build_capture({0: ([1000, 2000], [100], 1)})
        cursor = self.cursor(cap)
        self.run_ticks(cursor, 400)
        step(C.byref(cursor), 40)
        self.assertEqual(self.run_ticks(cursor, 2000), [(1000, 2), (2000, 6)])
        self.assertEqual(cursor.state.phase, 400)
        step(C.byref(cursor), 0)
        self.assertEqual((cursor.state.turn, cursor.state.phase), (2, 400))

    def test_same_track_does_not_repeat_consumed_edge(self):
        cursor = self.cursor(self.load(scp()))
        self.run_ticks(cursor, 160)
        before = bytes(cursor)
        step(C.byref(cursor), 0)
        self.assertEqual(bytes(cursor), before)
        self.assertEqual(self.run_ticks(cursor, 320), [(480, 1)])

    def test_phase_mapping_uses_checked_full_width_arithmetic(self):
        randomizer = random.Random(4551)
        for _ in range(150):
            a = randomizer.randrange(1, MAX)
            b = randomizer.randrange(1, MAX)
            phase = randomizer.randrange(a)
            cap = self.build_capture({0: ([a], [], 1), 1: ([b], [], 1)})
            cursor = self.cursor(cap)
            cursor.state.phase = phase
            self.assertEqual(step(C.byref(cursor), 1), 0)
            self.assertEqual(cursor.state.phase, phase * b // a)

    def test_random_partitioning_does_not_change_trace(self):
        cap = self.build_capture({0: ([1000, 1301, 799], [1, 100, 300, 700, 333, 1111], 1)})
        reference = self.cursor(cap)
        expected = self.run_ticks(reference, 31000)
        for seed in range(20):
            cursor = self.cursor(cap)
            self.assertEqual(self.run_ticks(cursor, 31000, seed), expected)
            self.assertEqual(bytes(cursor.state), bytes(reference.state))

    def test_random_captures_match_independent_timestamp_oracle(self):
        randomizer = random.Random(11821)
        for case in range(100):
            durations = [randomizer.randrange(1000, 9000) for _ in range(randomizer.randrange(1, 6))]
            unit = randomizer.randrange(1, 9)
            words = [randomizer.randrange(1, 251) for _ in range(100)]
            if case % 3 == 0:
                words[randomizer.randrange(len(words))] = 0
            cap = self.build_capture({0: (durations, words, unit)})
            native_time = 0
            timestamps = []
            for word in words:
                native_time += (word if word else 65536) * unit
                if word:
                    timestamps.append(native_time)
            boundaries = [0]
            for duration in durations:
                boundaries.append(boundaries[-1] + duration)
            expected = []
            elapsed = 0
            for turn in range(len(durations) * 3):
                rev = turn % len(durations)
                for pulse in timestamps:
                    if boundaries[rev] <= pulse < boundaries[rev + 1]:
                        expected.append((elapsed + pulse - boundaries[rev], 1))
                elapsed += durations[rev]
                expected.append((elapsed, 6 if rev + 1 == len(durations) else 2))
                if rev + 1 == len(durations) and boundaries[-1] in timestamps:
                    expected.append((elapsed, 1))
            # Include any edge exactly at the next window start, as the
            # scheduler consumes zero-delay events at the endpoint too.
            if boundaries[0] in timestamps:
                expected.append((elapsed, 1))
            with self.subTest(case=case):
                self.assertEqual(self.run_ticks(self.cursor(cap), elapsed, case), expected)

    def test_360_rpm_flag_keeps_measured_period(self):
        cap = self.load(scp({0: [(6666667, [3, 4])]}, flags=5))
        cursor = self.cursor(cap)
        self.assertEqual(cursor.state.duration, 13333334)
        self.assertEqual(self.run_ticks(cursor, 13333334), [(6, 1), (14, 1), (13333334, 6)])

    def test_long_run_has_no_per_interval_rounding_drift(self):
        cap = self.build_capture({0: ([1001, 997], [1, 500, 501, 400], 1)})
        cursor = self.cursor(cap)
        self.run_ticks(cursor, 1998 * 10000)
        self.assertEqual((cursor.state.turn, cursor.state.phase, cursor.state.elapsed), (20000, 0, 19980000))

    def test_cursor_restore_mid_interval_matches_uninterrupted_trace(self):
        cap = self.load(scp({0: [(1000, [100]), (1200, [1100])]}))
        for elapsed in [0, 199, 200, 201, 1999, 2000, 2400, 4399, 4400]:
            with self.subTest(elapsed=elapsed):
                cursor = self.cursor(cap)
                self.run_ticks(cursor, elapsed)
                saved = State.from_buffer_copy(bytes(cursor.state))
                restored = Cursor()
                self.assertEqual(restore(C.byref(restored), cap, C.byref(saved)), 0)
                self.assertEqual(self.run_ticks(restored, 7000), self.run_ticks(cursor, 7000))
                self.assertEqual(bytes(restored.state), bytes(cursor.state))

    def test_restore_motor_off_does_not_restart_spindle(self):
        cap = self.load(scp())
        cursor = self.cursor(cap)
        self.run_ticks(cursor, 100)
        motor(C.byref(cursor), 0)
        saved = State.from_buffer_copy(bytes(cursor.state))
        restored = Cursor()
        self.assertEqual(restore(C.byref(restored), cap, C.byref(saved)), 0)
        self.assertFalse(restored.state.motor)
        self.assertEqual(restored.state.phase, 100)

    def test_invalid_restore_is_atomic(self):
        cap = self.load(scp())
        cursor = self.cursor(cap)
        original = bytes(cursor)
        for key, value in [('duration', 0), ('duration', 99), ('phase', 16000000),
                           ('motor', 2), ('inclusive', 2), ('halftrack', 84)]:
            state = State.from_buffer_copy(bytes(cursor.state))
            setattr(state, key, value)
            self.assertEqual(restore(C.byref(cursor), cap, C.byref(state)), -1)
            self.assertEqual(bytes(cursor), original)

    def test_advance_cannot_skip_an_edge(self):
        cursor = self.cursor(self.load(scp()))
        original = bytes(cursor)
        kind = U32()
        self.assertEqual(advance(C.byref(cursor), 161, C.byref(kind)), -5)
        self.assertEqual(bytes(cursor), original)

    def test_elapsed_time_overflow_is_atomic(self):
        cursor = self.cursor(self.load(scp()))
        cursor.state.elapsed = MAX - 10
        before = bytes(cursor)
        self.assertEqual(advance(C.byref(cursor), 20, C.byref(U32())), -5)
        self.assertEqual(bytes(cursor), before)

    def test_revolution_counter_overflow_is_atomic(self):
        cap = self.build_capture({0: ([10], [], 1)})
        cursor = self.cursor(cap)
        cursor.state.turn = MAX
        before = bytes(cursor)
        self.assertEqual(advance(C.byref(cursor), 10, C.byref(U32())), -5)
        self.assertEqual(bytes(cursor), before)

    def test_checkpoint_seek_keeps_edge_at_exact_boundary(self):
        cap = self.build_capture({0: ([10000], [1] * 3000, 1)})
        scan, pulse = Scan(), U64()
        for value in [1, 1023, 1024, 1025, 2048, 3000]:
            self.assertEqual(seek(track(cap, 0), value, 1, C.byref(scan), C.byref(pulse)), 1)
            self.assertEqual(pulse.value, value)
        self.assertEqual(seek(track(cap, 0), 1024, 0, C.byref(scan), C.byref(pulse)), 1)
        self.assertEqual(pulse.value, 1025)

    def test_long_overflow_gap_uses_native_time(self):
        words = [0] * 20000 + [100]
        target = 20000 * 65536 + 100
        cap = self.build_capture({0: ([target + 1000], words, 1)})
        self.assertEqual(self.run_ticks(self.cursor(cap), target), [(target, 1)])

    def test_file_and_in_memory_provider_have_identical_trace(self):
        words = [10, 500, 700, 901]
        file_cap = self.load(scp({0: [(1000, words[:2]), (1200, words[2:])]}))
        memory_cap = self.build_capture({0: ([2000, 2400], words, 2)})
        self.assertEqual(self.run_ticks(self.cursor(file_cap), 20000),
                         self.run_ticks(self.cursor(memory_cap), 20000))

    def test_builder_is_immutable_after_sealing(self):
        cap = self.build_capture({0: ([1000], [10], 1)})
        self.assertTrue(is_sealed(cap))
        self.assertEqual(add(cap, 1, (U64 * 1)(1000), (C.c_uint16 * 1)(10), 1, 1), -1)

    def test_unsealed_capture_cannot_bind_a_cursor(self):
        cap = self.build_capture({0: ([1000], [], 1)}, sealed=False)
        self.assertFalse(track(cap, 0))
        self.assertEqual(bind(C.byref(Cursor()), cap, 0), -1)

    def test_builder_invalid_revolution_counts(self):
        for count in [0, 256, 0xffffffff]:
            self.assertFalse(create(count, 1))
        self.assertFalse(create(1, 2))

    def test_builder_rejects_duplicate_track(self):
        cap = self.build_capture({0: ([1000], [], 1)}, sealed=False)
        self.assertEqual(add(cap, 0, (U64 * 1)(1000), None, 0, 1), -1)

    def test_builder_rejects_word_limit_before_reading_input(self):
        cap = create(1, 1)
        self.captures.append(cap)
        self.assertEqual(add(cap, 0, (U64 * 1)(1000), (C.c_uint16 * 1)(1), 32000001, 1), -3)
        self.assertFalse(track(cap, 0))

    def test_builder_rejects_zero_and_overflowing_durations(self):
        cap = create(2, 1)
        self.captures.append(cap)
        for durations in [[0, 1], [MAX, 1]]:
            self.assertEqual(add(cap, 0, (U64 * 2)(*durations), None, 0, 1), -1)

    def test_builder_rejects_overflowing_flux_sum_without_leaks(self):
        cap = create(1, 1)
        self.captures.append(cap)
        self.assertEqual(add(cap, 0, (U64 * 1)(MAX), (C.c_uint16 * 65537)(), 65537, 0xffffffff), -1)
        self.assertEqual(word_count(cap), 0)
        self.assertEqual(live(), 1)

    def test_missing_track_bind_uses_explicit_nominal_period(self):
        cap = self.load(scp({}))
        cursor = self.cursor(cap)
        self.assertEqual(cursor.state.duration, 16000000)
        self.assertEqual(self.run_ticks(cursor, 16000000), [(16000000, 6)])

    def test_legacy_166_and_164_tables(self):
        for entries in [164, 166]:
            cap = self.load(scp(entries=entries))
            self.assertEqual(self.run_ticks(self.cursor(cap), 1000), [(160, 1), (480, 1), (960, 1)])

    def test_legacy_table_needs_matching_first_record(self):
        data = scp(entries=166)
        data[680] = 0
        self.load(checksum(data), -1)

    def test_unknown_short_table_is_rejected(self):
        self.load(scp(entries=160) + bytes(100), -1)

    def test_sparse_halftracks_and_last_supported_track(self):
        cap = self.load(scp({1: [(1000, [1])], 83: [(1000, [2])]}, start=1))
        self.assertFalse(track(cap, 0))
        self.assertTrue(track(cap, 1))
        self.assertTrue(track(cap, 83))
        self.assertEqual(self.run_ticks(self.cursor(cap, 83), 10), [(4, 1)])

    def test_255_revolutions_are_supported(self):
        cap = self.load(scp({0: [(1000, [1])] + [(1000, [1000])] * 254}))
        self.assertEqual(revs(cap), 255)
        cursor = self.cursor(cap)
        self.run_ticks(cursor, 510000)
        self.assertEqual((cursor.state.turn, cursor.state.phase), (255, 0))

    def test_read_write_mode_is_still_read_only_input(self):
        self.load(scp(flags=0x11))

    def test_normalized_multiturn_preservation_is_valid(self):
        self.load(scp({0: [(1000, [1]), (1000, [1000])]}, flags=9))

    def test_explicit_16_bit_width_and_legacy_heads(self):
        self.load(scp(width=16, heads=0))

    def test_bad_checksum(self):
        data = scp();data[12] ^= 1
        self.load(data, -1)

    def test_invalid_recognized_header_fields(self):
        for offset, value in [(4, 0x80), (4, 1), (5, 0), (6, 1), (7, 84),
                              (8, 0x41), (9, 8), (10, 2), (10, 255)]:
            with self.subTest(offset=offset, value=value):
                data = scp();data[offset] = value
                self.load(checksum(data), -1)

    def test_non_scp_is_not_a_recognized_error(self):
        for data in [b'', b'S', b'SC', b'P64-1541', bytes(688)]:
            self.load(data, 1)

    def test_recognized_truncated_scp_is_invalid(self):
        self.load(b'SCP', -1)
        self.load(scp()[:100], -1)

    def test_invalid_tdh_and_bounds(self):
        for offset, value in [(688, 0), (691, 1)]:
            data = scp();data[offset] = value
            self.load(checksum(data), -1)
        for position, value in [(16, 0xffffffff), (692, 0), (696, 0xffffffff), (700, 0), (700, 0xffffffff)]:
            data = scp();struct.pack_into('<I', data, position, value)
            self.load(checksum(data), -1)

    def test_hidden_track_outside_header_range(self):
        data = scp({0: [(1000, [1])], 2: [(1000, [2])]})
        data[7] = 0
        self.load(checksum(data), -1)

    def test_rw_requires_single_revolution_and_zero_checksum(self):
        self.load(scp({0: [(1000, [1]), (1000, [1000])]}, flags=0x11), -1)
        data = scp(flags=0x11);data[12] = 1
        self.load(data, -1)

    def test_out_of_order_track_records(self):
        cap = self.load(scp({2: [(1000, [2])], 0: [(1000, [1])]}))
        self.assertEqual(word_count(cap), 2)

    def test_skipped_tracks_do_not_require_records(self):
        self.load(scp({}, revolutions=5))

    def test_valid_utf8_footer_and_future_known_layout(self):
        self.load(metadata())
        self.load(metadata(revision=0x17))

    def test_invalid_footer_signature_revision_and_header(self):
        for offset, value in [(-1, 0), (-5, 0x15), (3, 0x17)]:
            data = metadata();data[offset] = value
            self.load(checksum(data), -1)

    def test_footer_string_must_follow_track_data(self):
        data = metadata();struct.pack_into('<I', data, len(data) - 48, 688)
        self.load(checksum(data), -1)

    def test_footer_string_bounds_and_null(self):
        data = metadata();offset, = struct.unpack_from('<I', data, len(data) - 48)
        for position, value in [(offset, 255), (offset + 1, 255), (offset + 2 + 5, 1)]:
            bad = bytearray(data);bad[position] = value
            self.load(checksum(bad), -1)

    def test_footer_rejects_embedded_null_and_invalid_utf8(self):
        data = metadata();offset, = struct.unpack_from('<I', data, len(data) - 48)
        for value in [0, 0x80, 0xc0, 0xff]:
            bad = bytearray(data);bad[offset + 2] = value
            self.load(checksum(bad), -1)

    def test_flagged_missing_footer(self):
        self.load(scp(flags=0x21), -1)

    def test_unicode_filename(self):
        self.load(scp(), name='native-\u30c6\u30b9\u30c8.scp')

    def test_read_only_file_attribute(self):
        path = OUT / 'readonly.scp'
        path.write_bytes(scp());path.chmod(0o444)
        cap = PTR()
        try:
            self.assertEqual(read(str(path).encode(), C.byref(cap)), 0)
            self.assertTrue(cap)
            self.captures.append(cap)
        finally:
            path.chmod(0o666)

    def test_parser_allocation_failures_release_everything(self):
        path = OUT / 'allocation-failures.scp'
        data = metadata();path.write_bytes(data)
        for number in range(50):
            fail_alloc(number)
            capture = PTR()
            rc = read(str(path).encode(), C.byref(capture))
            reset()
            if rc == 0:
                destroy(capture)
                self.assertEqual(live(), 0)
                break
            self.assertEqual(rc, -2)
            self.assertFalse(capture)
            self.assertEqual(live(), 0)
        else:
            self.fail('allocation failure test did not reach success')
        self.assertEqual(path.read_bytes(), data)

    def test_builder_allocation_failures_are_transactional(self):
        cap = create(1, 1)
        self.captures.append(cap)
        for number in range(3):
            fail_alloc(number)
            self.assertEqual(add(cap, 0, (U64 * 1)(1000), (C.c_uint16 * 1)(1), 1, 1), -2)
            reset()
            self.assertEqual(word_count(cap), 0)
            self.assertEqual(live(), 1)
        self.assertEqual(add(cap, 0, (U64 * 1)(1000), (C.c_uint16 * 1)(1), 1, 1), 0)

    def test_injected_read_failures_do_not_publish_partial_captures(self):
        path = OUT / 'read-failures.scp';path.write_bytes(metadata())
        for number in range(1, 25):
            fail_read(number)
            capture = PTR();rc = read(str(path).encode(), C.byref(capture))
            reset()
            if rc == 0:
                destroy(capture)
                self.assertEqual(live(), 0)
                break
            self.assertEqual(rc, -4)
            self.assertFalse(capture)
            self.assertEqual(live(), 0)
        else:
            self.fail('read failure test did not reach success')

    def test_seek_failures_do_not_publish_partial_captures(self):
        path = OUT / 'seek-failures.scp';path.write_bytes(metadata())
        for number in range(25):
            fail_seek(number)
            capture = PTR();rc = read(str(path).encode(), C.byref(capture))
            reset()
            if rc == 0:
                destroy(capture)
                break
            self.assertEqual(rc, -4)
            self.assertFalse(capture)
            self.assertEqual(live(), 0)

    def test_source_size_limit_checked_before_allocating(self):
        fake_size((1 << 30) + 1)
        self.load(scp(), -3)
        reset()

    def test_file_size_failure(self):
        fake_size(-1)
        self.load(scp(), -4)
        reset()

    def test_deterministic_mutation_corpus_has_no_leaks_or_partial_publication(self):
        randomizer = random.Random(71916)
        path = OUT / 'mutations.scp'
        seeds = [scp(), metadata(), scp({0: [(1000, [1]), (1200, [1100])]}), scp(entries=166)]
        rejected = accepted = 0
        for case in range(1000):
            data = bytearray(randomizer.choice(seeds))
            for _ in range(randomizer.randrange(1, 9)):
                data[randomizer.randrange(len(data))] = randomizer.randrange(256)
            if case % 4 == 0:
                del data[randomizer.randrange(len(data)):]
            elif case % 4 == 1:
                data += bytes(randomizer.randrange(64))
            if len(data) >= 16 and case % 2:
                data = checksum(data)
            path.write_bytes(data)
            capture = PTR()
            rc = read(str(path).encode(), C.byref(capture))
            if rc == 0:
                self.assertTrue(capture)
                destroy(capture)
                accepted += 1
            else:
                self.assertFalse(capture)
                rejected += 1
            self.assertEqual(live(), 0, f'mutation {case} leaked')
            self.assertEqual(path.read_bytes(), data)
        self.assertGreater(rejected, 900)
        self.assertGreater(accepted, 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
