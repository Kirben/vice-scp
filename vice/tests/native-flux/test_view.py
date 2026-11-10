"""Independent GCR encoding exercises the production private flux sector view.

Run with the library built by build-lifecycle.py and an output directory.
"""
import ctypes as C
import json
import test_native_flux as core

view_create = core.api('flux_view_create', core.PTR, core.PTR)
view_destroy = core.api('flux_view_destroy', None, core.PTR)
view_read = core.api('flux_view_read', C.c_int, core.PTR, core.U32, core.U32, core.PTR)
ENCODE = [10, 11, 18, 19, 14, 15, 22, 23, 9, 25, 26, 27, 13, 29, 30, 21]
results = {}


def block(data):
    return ''.join(format(ENCODE[b >> 4], '05b') + format(ENCODE[b & 15], '05b') for b in data)


def sector(payload, track=1, sector=0, header_bad=False, data_bad=False, symbol_bad=False):
    header = [8, sector ^ track ^ 1 ^ 2 ^ int(header_bad), sector, track, 1, 2, 15, 15]
    check = 0
    for b in payload:
        check ^= b
    encoded = block([7] + list(payload) + [check ^ int(data_bad), 0, 0])
    if symbol_bad:
        encoded = encoded[:100] + '00000' + encoded[105:]
    return '1' * 40 + block(header) + '0101' * 18 + '1' * 40 + encoded + '0101' * 20


def capture(turns, track=1, rotate=0, duration=None):
    zone = int(track < 31) + int(track < 25) + int(track < 18)
    bit_ticks = (16 - zone) * 20
    words = []
    durations = []
    previous = 0
    elapsed = 0
    for bits in turns:
        bits = '0101' * 40 + bits + '0101' * 40
        if rotate:
            bits = bits[rotate:] + bits[:rotate]
        length = duration or len(bits) * bit_ticks
        for i, bit in enumerate(bits):
            if bit != '1':
                continue
            at = elapsed + (i + 1) * bit_ticks
            delta = at - previous
            while delta > 65535:
                words.append(0)
                delta -= 65536
            words.append(delta)
            previous = at
        elapsed += length
        durations.append(length)
    cap = core.create(len(turns), 1)
    assert cap
    assert core.add(cap, (track - 1) * 2, (core.U64 * len(turns))(*durations),
                    (C.c_uint16 * len(words))(*words), len(words), 1) == 0
    assert core.seal(cap) == 0
    return cap


def check(name, turns, expected=0, payload=None, track=1, read_track=None, sector_no=0, rotate=0, duration=None):
    core.reset()
    cap = capture(turns, track, rotate, duration)
    view = view_create(cap)
    assert view
    buffer = (C.c_ubyte * 256)(*([0xA5] * 256))
    try:
        for repeat in range(2):
            rc = view_read(view, read_track or track, sector_no, buffer)
            assert rc == expected, (name, rc, expected)
            assert bytes(buffer) == (payload if expected == 0 else bytes([0xA5]) * 256), name
        results[name] = True
    finally:
        view_destroy(view)
        core.destroy(cap)
    assert core.live() == 0, name


payload = bytes(range(256))
other = bytes([0x42]) * 256
good = sector(payload)
check('valid_and_cached', [good], payload=payload)
check('wrap_across_capture_origin', [good], payload=payload, rotate=900)
check('bad_header_checksum', [sector(payload, header_bad=True)], expected=27)
check('bad_data_checksum', [sector(payload, data_bad=True)], expected=23)
check('invalid_gcr_symbol', [sector(payload, symbol_bad=True)], expected=24)
check('wrong_header_track', [sector(payload, track=2)], expected=20)
check('missing_track', [good], read_track=2, expected=20)
check('missing_sector', [good], sector_no=1, expected=20)
check('invalid_track', [good], read_track=43, expected=66)
check('invalid_sector', [good], sector_no=21, expected=66)
check('later_valid_turn', [sector(other, data_bad=True), good], payload=payload)
check('first_valid_turn_wins', [good, sector(other)], payload=payload)
for track in [18, 25, 31, 42]:
    check('zone_read_' + str(track), [sector(payload, track=track)], track=track, payload=payload)
    check('zone_geometry_' + str(track), [good], track=track, sector_no=21, expected=66)
core.reset()
cap = capture([good])
before = core.live()
core.fail_alloc(0)
assert not view_create(cap)
assert core.live() == before
core.reset()
core.destroy(cap)
assert core.live() == 0
results['allocation_failure_cleanup'] = True
(core.OUT / 'view-results.json').write_text(json.dumps(results, indent=2))
print('Private flux sector view:', len(results), 'checks passed')
