"""Generated two-unit acceptance checks; invoked by test_native_drive.py."""
from pathlib import Path
import hashlib
import json
import struct
import subprocess


def run(Session, out, image, success, basic, env, app, disk_tool):
    def digest(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()

    payload = out / 'unit9.prg'
    payload.write_bytes(b'\x00\x20' + b'\x99' * 16)
    g64 = out / 'unit9.g64'
    tool = disk_tool.resolve() if disk_tool else app / 'c1541.exe'
    made = subprocess.run([str(tool), '-format', 'unitnine,09', 'g64', str(g64),
                           '-write', str(payload), 'success'], capture_output=True, env=env, timeout=30)
    assert made.returncode == 0, made.stderr
    raw = g64.read_bytes()
    count = raw[9]
    data = bytearray(688)
    tracks = []
    for ht in range(count):
        offset, = struct.unpack_from('<I', raw, 12 + ht * 4)
        if not offset:
            continue
        size, = struct.unpack_from('<H', raw, offset)
        zone, = struct.unpack_from('<I', raw, 12 + count * 4 + ht * 4)
        assert zone <= 3
        bits = raw[offset + 2:offset + 2 + size]
        words = []
        previous = 0
        for bit in range(size * 8):
            at = (bit + 1) * (16 - zone) * 20
            if at > 16000000 or not bits[bit // 8] & (0x80 >> (bit % 8)):
                continue
            delta = (at - previous) // 2
            while delta > 65535:
                words.append(0)
                delta -= 65536
            assert delta > 0
            words.append(delta)
            previous = at
        struct.pack_into('<I', data, 16 + ht * 4, len(data))
        data += b'TRK' + bytes([ht]) + struct.pack('<III', 8000000, len(words), 16)
        data += struct.pack('>' + 'H' * len(words), *words)
        tracks.append(ht)
    data[:16] = struct.pack('<3s9BI', b'SCP', 0x17, 0, 1, min(tracks), max(tracks), 1, 0, 1, 0, 0)
    struct.pack_into('<I', data, 12, sum(data[16:]) & 0xffffffff)
    second = out / 'unit9.scp'
    second.write_bytes(data)
    second.chmod(0o444)
    before = {path: digest(path) for path in [image, second]}

    # Actual KERNAL LOADs from units 8 and 9 must return different file bytes.
    # Execution stays at $c000 while LOAD overwrites the idle BASIC program.
    def loader(units, origin):
        code = bytearray()
        branches = []
        primary = success.read_bytes()
        for unit in units:
            address, value = (int.from_bytes(primary[:2], 'little'), primary[2]) if unit == 8 else (0x2000, 0x99)
            # Clear the expected destination before each LOAD: a swapped disk
            # cannot pass by leaving data behind from the preceding transfer.
            code += bytes([0xa9, value ^ 255, 0x8d, address & 255, address >> 8])
            code += bytes.fromhex('a907a280a0c120bdff')
            code += bytes([0xa9, 1, 0xa2, unit, 0xa0, 1, 0x20, 0xba, 0xff])
            code += bytes.fromhex('a900a200a00020d5ffb000')
            branches.append(len(code) - 1)
            code += bytes([0xad, address & 255, address >> 8, 0xc9, value, 0xd0, 0])
            branches.append(len(code) - 1)
        code += bytes.fromhex('a9008dffd7') + bytes([0x4c, origin & 255, origin >> 8])
        failure = len(code)
        code += bytes.fromhex('a9018dffd7') + bytes([0x4c, origin & 255, origin >> 8])
        for branch in branches:
            delta = failure - branch - 1
            assert 0 <= delta < 128
            code[branch] = delta
        return code

    ram = bytearray(basic + bytes.fromhex('4c0d08'))
    ram += bytes(0xc000 - 0x0801 - len(ram))
    ram += loader([8, 9], 0xc000)
    ram += bytes(0xc100 - 0x0801 - len(ram))
    ram += loader([9], 0xc100)
    ram += bytes(0xc180 - 0x0801 - len(ram)) + b'SUCCESS'
    program = out / 'two-unit-loader.prg'
    program.write_bytes(b'\x01\x08' + ram)
    results = {}

    def module(data, unit):
        start = data.index(('FLUX%d:0' % unit).encode() + b'\0')
        size, = struct.unpack_from('<I', data, start + 18)
        assert size >= 22 and start + size <= len(data)
        return data[start + 22:start + size]

    def resource(session, name, value):
        raw = name.encode()
        width = max(1, (value.bit_length() + 7) // 8)
        return session.command(0x52, bytes([1, len(raw)]) + raw + bytes([width]) + value.to_bytes(width, 'little'))[0]

    session = Session('two-unit-save', program=program, additional_disks={9: second})
    try:
        embedded = session.save('embedded.vsf', 1)
        external = session.save('external.vsf', 0)
        original = external.read_bytes()
        assert module(original, 8)[1:21] != module(original, 9)[1:21]
        assert session.restore(external) == 0
        restored = session.save('external-roundtrip.vsf', 0).read_bytes()
        for unit in [8, 9]:
            assert module(original, unit) == module(restored, unit)
        session.load_program()
        results['distinct_captures_both_true_drive_load'] = True
        results['two_unit_external_exact_roundtrip'] = True
    finally:
        session.close()
    print('Two native units loaded distinct programs; external snapshot matched both.', flush=True)

    offline = []
    try:
        for path in [image, second]:
            renamed = path.with_suffix('.offline')
            path.rename(renamed)
            offline.append((path, renamed))
        session = Session('two-unit-embedded-offline', disk=None, program=program,
                          additional_disks={9: None})
        try:
            assert session.restore(embedded) == 0
            restored = session.save('embedded-roundtrip.vsf', 1).read_bytes()
            for unit in [8, 9]:
                assert module(embedded.read_bytes(), unit) == module(restored, unit)
            session.load_program()
            results['two_unit_embedded_offline_exact_roundtrip_and_load'] = True
        finally:
            session.close()
    finally:
        for path, renamed in offline:
            renamed.rename(path)
    print('Both embedded captures restored and loaded with source files unavailable.', flush=True)

    session = Session('two-unit-active-unit9', program=program, additional_disks={9: second})
    try:
        rc, _ = session.command(0x12, struct.pack('<HHBBBBB', 0xffd5, 0xffd5, 1, 1, 4, 0, 0))
        assert rc == 0
        rc, _ = session.command(0x32, bytes([0, 1, 0, 3, 3, 0, 0xc0]))
        assert rc == 0
        # Stop at each KERNAL LOAD: first unit 8, then unit 9.
        for _ in range(2):
            rc, _ = session.command(0xaa)
            assert rc == 0
            session.wait_stop(0xffd5)
            session.events = []
        for _ in range(5):
            rc, _ = session.command(0x71, b'\0' + struct.pack('<H', 20000))
            assert rc == 0
            session.wait_stop()
            session.events = []
            active = session.save('active.vsf', 1)
            if module(active.read_bytes(), 9)[54] == 1:
                break
        else:
            raise AssertionError('Unit 9 motor did not start during its LOAD')
        assert session.restore(active) == 0
        roundtrip = session.save('active-roundtrip.vsf', 1).read_bytes()
        for unit in [8, 9]:
            assert module(active.read_bytes(), unit) == module(roundtrip, unit)
        session.load_program(None)
        results['active_unit9_snapshot_preserves_both_states_and_completes_load'] = True
    finally:
        session.close()
    print('Active unit-9 snapshot preserved both units and completed LOAD.', flush=True)

    session = Session('two-unit-detach', program=program, additional_disks={9: second})
    try:
        prior = module(session.save('before.vsf', 0).read_bytes(), 9)
        # Changing the filesystem device back to FS invokes normal image detach.
        assert resource(session, 'FileSystemDevice8', 0) == 0
        assert resource(session, 'FileSystemDevice8', 1) == 0
        detached = session.save('detached.vsf', 0).read_bytes()
        assert b'FLUX8:0\0' not in detached
        assert module(detached, 9) == prior
        assert resource(session, 'TrapDevice8', 1) == 0
        assert resource(session, 'TrapDevice9', 1) != 0
        session.load_program(0xc100)
        results['unit8_detach_preserves_unit9_state_and_load'] = True
        results['trap_guard_tracks_each_units_attachment'] = True
    finally:
        session.close()
    print('Detaching unit 8 preserved unit 9 state and subsequent native LOAD.', flush=True)

    session = Session('two-unit-model-guards', program=program, additional_disks={9: second})
    try:
        prior = session.save('before.vsf', 0).read_bytes()
        assert resource(session, 'Drive8Type', 1542) == 0
        assert resource(session, 'Drive9Type', 1542) == 0
        assert resource(session, 'Drive8Type', 1581) != 0
        assert resource(session, 'Drive9Type', 1541) != 0
        checked = session.save('after.vsf', 0).read_bytes()
        for unit in [8, 9]:
            assert module(checked, unit) == module(prior, unit)
        session.load_program()
        results['model_guards_preserve_both_units_and_load'] = True
    finally:
        session.close()

    session = Session('detached-unit-model', disk=None, program=program, additional_disks={9: second})
    try:
        prior = module(session.save('before.vsf', 0).read_bytes(), 9)
        assert resource(session, 'Drive8Type', 1541) != 0
        assert resource(session, 'Drive9Type', 1581) != 0
        checked = session.save('after.vsf', 0).read_bytes()
        assert module(checked, 9) == prior
        session.load_program(0xc100)
        results['other_units_model_change_rejected_without_disrupting_native_load'] = True
    finally:
        session.close()

    session = Session('all-native-detached-model', program=program, additional_disks={9: second})
    try:
        for unit in [8, 9]:
            assert resource(session, 'FileSystemDevice%d' % unit, 0) == 0
            assert resource(session, 'FileSystemDevice%d' % unit, 1) == 0
        detached = session.save('detached.vsf', 0).read_bytes()
        assert b'FLUX8:0' not in detached and b'FLUX9:0' not in detached
        assert resource(session, 'Drive8Type', 1541) == 0
        assert resource(session, 'Drive9Type', 1581) == 0
        results['model_changes_allowed_after_all_native_media_detached'] = True
    finally:
        session.close()

    for name, disk8, disk9 in [('swapped', second, image), ('missing9', image, None)]:
        session = Session('two-unit-external-' + name, disk=disk8, program=program,
                          additional_disks={9: disk9})
        try:
            assert session.restore(external) != 0
            results['external_' + name + '_rejected'] = True
        finally:
            session.close()

    for path, expected in before.items():
        assert digest(path) == expected
    results['generated_images_unchanged'] = True
    results['complete'] = True
    (out / 'multiunit-results.json').write_text(json.dumps(results, indent=2) + '\n')
    print('TWO-UNIT NATIVE ACCEPTANCE COMPLETE', flush=True)
