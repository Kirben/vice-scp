"""Generated 360 RPM media with a known program, live state and snapshot checks."""
import hashlib
import json
import struct
import subprocess


def run(Session, out, image, success, env, app):
    source = image.read_bytes()
    data = bytearray(688)
    for ht in range(84):
        offset, = struct.unpack_from('<I', source, 16 + ht * 4)
        if not offset:
            continue
        duration, count, start = struct.unpack_from('<III', source, offset + 4)
        raw_time = previous = 0
        words = []
        for word, in struct.iter_unpack('>H', source[offset + start:offset + start + 2 * count]):
            raw_time += word or 65536
            if not word:
                continue
            position = (raw_time * 5 + 3) // 6
            delta = position - previous
            while delta > 65535:
                words.append(0)
                delta -= 65536
            assert delta > 0
            words.append(delta)
            previous = position
        struct.pack_into('<I', data, 16 + ht * 4, len(data))
        data += b'TRK' + bytes([ht]) + struct.pack('<III', (duration * 5 + 3) // 6, len(words), 16)
        data += struct.pack('>' + 'H' * len(words), *words)
    data[:16] = source[:16]
    data[8] |= 0x84
    struct.pack_into('<I', data, 12, sum(data[16:]) & 0xffffffff)
    disk = out / 'captured-360.scp'
    disk.write_bytes(data)
    disk.chmod(0o444)
    original_hash = hashlib.sha256(data).hexdigest()
    results = {}
    extracted = out / 'extracted-360.prg'
    read = subprocess.run([str(app / 'c1541.exe'), '-attach', str(disk), '-list',
                           '-read', 'success', str(extracted)], env=env, capture_output=True, timeout=45)
    assert read.returncode == 0 and b'blocks free.' in read.stdout.lower(), read.stdout
    assert extracted.read_bytes() == success.read_bytes()
    results['scaled_private_directory_and_exact_extraction'] = True

    def module(path):
        raw = path.read_bytes()
        start = raw.index(b'FLUX8:0' + bytes([0]))
        size, = struct.unpack_from('<I', raw, start + 18)
        assert raw[start + 16:start + 18] == b'\x01\x01'
        return start, raw[start + 22:start + size]

    def resource(session, name, val):
        key = name.encode()
        return session.command(0x52, bytes([1, len(key)]) + key + bytes([1, val]))[0]

    unflagged = out / 'indexed-360-no-rpm-flag.scp'
    inferred = bytearray(data)
    inferred[8] &= ~4
    unflagged.write_bytes(inferred)
    unflagged.chmod(0o444)
    session = Session('inferred-360', disk=unflagged)
    try:
        saved = session.save('inferred.vsf', 0)
        assert module(saved)[1][57] == 6
        assert session.restore(saved) == 0
        session.load_program()
        assert unflagged.read_bytes() == inferred
        results['unflagged_indexed_360_native_load_and_snapshot'] = True
    finally:
        session.close()
    extracted = out / 'extracted-inferred.prg'
    subprocess.run([str(app / 'c1541.exe'), '-attach', str(unflagged),
                    '-read', 'success', str(extracted)], env=env,
                   capture_output=True, timeout=45, check=True)
    assert extracted.read_bytes() == success.read_bytes()
    results['unflagged_indexed_360_sector_view'] = True

    for model in [1541, 1542]:
        session = Session('auto-' + str(model), disk=disk, model=model)
        try:
            before = session.save('before.vsf', 0)
            _, state = module(before)
            assert state[57] == 6 and state[58] < 6
            assert resource(session, 'Drive8FluxSpeed', 1) != 0
            assert resource(session, 'Drive8FluxSpeed', 2) != 0
            assert resource(session, 'Drive8FluxSpeed', 0) == 0
            assert resource(session, 'Drive9FluxSpeed', 1) == 0
            assert module(session.save('after.vsf', 0))[1] == state
            assert session.restore(before) == 0
            assert module(session.save('restored.vsf', 0))[1] == state
            session.load_program()
            results['auto_native_load_and_exact_external_state_' + str(model)] = True
        finally:
            session.close()

    session = Session('captured-clock', disk=disk, flux_speed=1)
    try:
        assert module(session.save('captured.vsf', 0))[1][57:59] == b'\x05\x00'
        assert resource(session, 'Drive8FluxSpeed', 0) != 0
        assert resource(session, 'FileSystemDevice8', 0) == 0
        assert resource(session, 'FileSystemDevice8', 1) == 0
        assert resource(session, 'Drive8FluxSpeed', 0) == 0
        results['captured_clock_override_and_detach_guard'] = True
    finally:
        session.close()

    session = Session('active-360', disk=disk)
    try:
        rc, _ = session.command(0x12, struct.pack('<HHBBBBB', 0xffd5, 0xffd5, 1, 1, 4, 1, 0))
        assert rc == 0
        rc, _ = session.command(0x32, bytes([0, 1, 0, 3, 3, 0, 0xc0]))
        assert rc == 0
        rc, _ = session.command(0xaa)
        assert rc == 0
        session.wait_stop(0xffd5)
        for attempt in range(12):
            session.events = []
            rc, _ = session.command(0x71, b'\0' + struct.pack('<H', 20001))
            assert rc == 0
            session.wait_stop()
            _, state = module(session.save('probe.vsf', 0))
            if state[54] and state[58]:
                break
        else:
            raise AssertionError('No moving 360 RPM cursor with fractional phase')
        active = session.save('active.vsf', 1)
        _, state = module(active)
        assert state[57] == 6 and state[58] > 0
        assert session.restore(active) == 0
        assert module(session.save('roundtrip.vsf', 1))[1] == state
        session.load_program(None)
        results['active_scaled_snapshot_exact_fraction_and_load'] = True
    finally:
        session.close()

    offline = disk.with_suffix('.offline')
    disk.rename(offline)
    try:
        session = Session('offline-360', disk=None, flux_speed=1)
        try:
            assert session.restore(active) == 0
            assert module(session.save('roundtrip.vsf', 1))[1] == state
            session.load_program(None)
            results['embedded_scaled_snapshot_offline_preserves_speed_and_load'] = True
        finally:
            session.close()
    finally:
        offline.rename(disk)

    start, _ = module(active)
    for name, field, value in [('speed', 57, 7), ('source-phase', 58, 6), ('reference-phase', 56, 25)]:
        raw = bytearray(active.read_bytes())
        raw[start + 22 + field] = value
        bad = out / ('invalid-' + name + '.vsf')
        bad.write_bytes(raw)
        session = Session('invalid-' + name, disk=None)
        try:
            assert session.restore(bad) != 0
            results['invalid_scaled_' + name + '_rejected'] = True
        finally:
            session.close()

    assert hashlib.sha256(disk.read_bytes()).hexdigest() == original_hash
    assert image.read_bytes() == source
    results['generated_sources_unchanged'] = True
    results['complete'] = True
    (out / 'capture-speed-results.json').write_text(json.dumps(results, indent=2) + '\n')
    print('CAPTURE SPEED ACCEPTANCE COMPLETE', flush=True)
