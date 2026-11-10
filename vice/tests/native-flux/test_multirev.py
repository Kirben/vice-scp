"""Generated multi-revolution and same-unit replacement acceptance checks."""
import hashlib
import json
import struct
import subprocess
import socket


def run(Session, out, image, success, basic, env, app):
    results = {}
    observations = {}
    originals = [image]

    def multirev(source, name, turns):
        raw = source.read_bytes()
        assert raw[5] == 1
        data = bytearray(688)
        for ht in range(168):
            offset, = struct.unpack_from('<I', raw, 16 + ht * 4)
            if not offset:
                continue
            duration, count, start = struct.unpack_from('<III', raw, offset + 4)
            words = raw[offset + start:offset + start + count * 2]
            edges = []
            time = 0
            for word, in struct.iter_unpack('>H', words):
                time += word or 65536
                if word:
                    edges.append(time)
            struct.pack_into('<I', data, 16 + ht * 4, len(data))
            track = bytearray(b'TRK' + bytes([ht]) + bytes(12 * len(turns)))
            origin = previous = 0
            for rev, (ticks, blank) in enumerate(turns):
                ticks = ticks or duration
                intervals = []
                for edge in ([] if blank else edges):
                    assert edge <= ticks
                    delta = origin + edge - previous
                    while delta > 65535:
                        intervals.append(0)
                        delta -= 65536
                    assert delta > 0
                    intervals.append(delta)
                    previous = origin + edge
                samples = struct.pack('>' + 'H' * len(intervals), *intervals)
                struct.pack_into('<III', track, 4 + rev * 12, ticks or duration,
                                 len(samples) // 2, len(track))
                track += samples
                origin += ticks
            data += track
        data[:16] = raw[:16]
        data[5] = len(turns)
        data[8] |= 0x80  # Third-party flux creator (SCP specification bit 7).
        struct.pack_into('<I', data, 12, sum(data[16:]) & 0xffffffff)
        target = out / name
        target.write_bytes(data)
        target.chmod(0o444)
        originals.append(target)
        return target

    repeated = multirev(image, 'two-good-turns.scp', [(0, False), (0, False)])
    recovery = multirev(image, 'blank-first-turn.scp', [(7840000, True), (0, False)])
    uneven = multirev(image, 'unequal-three-turns.scp',
                      [(0, False), (8080000, False), (8160000, False)])
    hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in originals}
    for disk in [repeated, recovery, uneven]:
        extracted = out / (disk.stem + '.prg')
        read = subprocess.run([str(app / 'c1541.exe'), '-attach', str(disk),
                               '-read', 'success', str(extracted)],
                              capture_output=True, env=env, timeout=60)
        assert read.returncode == 0 and extracted.read_bytes() == success.read_bytes(), read.stderr
        results[disk.stem + '_exact_sector_extraction'] = True
        if disk == recovery:
            # An alternating whole blank turn is an adverse capture, not a
            # promise that the drive ROM can recover every LOAD. Keep this
            # fixture for sector-view recovery; native DOS behavior is reported
            # separately rather than counted as a successful LOAD.
            for model in [1541, 1542]:
                session = Session('blank-turn-observation-' + str(model), disk=disk, model=model)
                try:
                    # The idle fixture's loader exits through DebugCart with
                    # 0 on success or 255 when KERNAL LOAD returns carry set.
                    rc, _ = session.command(0x32, bytes([0, 1, 0, 3, 3, 0, 0xc0]))
                    assert rc == 0
                    rc, _ = session.command(0xaa)
                    assert rc == 0
                    status = session.process.wait(timeout=40)
                    assert status in [0, 255], (status, session.log.read_text(errors='replace')[-2000:])
                    observations['blank_first_turn_native_' + str(model)] = {
                        'exit_code': status, 'load_succeeded': status == 0}
                finally:
                    session.close()
            continue
        for model in [1541, 1542]:
            session = Session(disk.stem + '-' + str(model), disk=disk, model=model)
            try:
                session.load_program()
                results[disk.stem + '_native_load_' + str(model)] = True
            finally:
                session.close()
        print(disk.name + ': extraction and both native drive models passed.', flush=True)

    # Replacing the mounted disk must change the capture identity; returning to
    # the first must restore that identity without restoring its cursor.
    def module(session, name, embed=0):
        raw = session.save(name, embed).read_bytes()
        at = raw.index(b'FLUX8:0' + bytes([0]))
        size, = struct.unpack_from('<I', raw, at + 18)
        payload = raw[at + 22:at + size]
        return payload

    def identity(session, name):
        return module(session, name)[1:21]

    session = Session('multi-turn-snapshots', disk=uneven)
    try:
        external = session.save('external.vsf', 0)
        prior = module(session, 'before.vsf')
        assert session.restore(external) == 0
        assert module(session, 'after.vsf') == prior
        embedded = session.save('embedded.vsf', 1)
        embedded_state = module(session, 'embedded-before.vsf', 1)
        results['multi_turn_external_exact_roundtrip'] = True
    finally:
        session.close()
    offline = uneven.with_suffix('.offline')
    uneven.rename(offline)
    try:
        session = Session('multi-turn-embedded-offline', disk=None)
        try:
            assert session.restore(embedded) == 0
            assert module(session, 'embedded-after.vsf', 1) == embedded_state
            session.load_program()
            results['multi_turn_embedded_offline_exact_roundtrip_and_load'] = True
        finally:
            session.close()
    finally:
        offline.rename(uneven)

    def attach(session, disk):
        with socket.create_connection(('127.0.0.1', session.remote_port), timeout=10) as connection:
            connection.settimeout(10)
            connection.sendall(('attach "' + disk.as_posix() + '" 8\n').encode())
            response = b''
            while not response.rstrip().endswith(b')'):
                part = connection.recv(8192)
                assert part
                response += part
            assert b'Failed' not in response and b'ERROR' not in response, response

    session = Session('same-unit-replacement', disk=repeated, remote_monitor=True)
    try:
        first = identity(session, 'first.vsf')
        attach(session, uneven)
        second = identity(session, 'second.vsf')
        assert first != second
        attach(session, repeated)
        assert identity(session, 'first-again.vsf') == first
        session.load_program()
        results['same_unit_replacement_identity_and_native_load'] = True
    finally:
        session.close()
    for path in originals:
        assert hashlib.sha256(path.read_bytes()).hexdigest() == hashes[path.name]
    results['all_generated_sources_unchanged'] = True
    results['complete'] = True
    (out / 'multirev-results.json').write_text(json.dumps(results, indent=2) + '\n')
    (out / 'multirev-observations.json').write_text(json.dumps(observations, indent=2) + '\n')
    print('MULTI-REVOLUTION AND REPLACEMENT ACCEPTANCE COMPLETE', flush=True)
