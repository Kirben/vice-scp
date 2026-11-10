"""Generated diagnostic matrix for blank-turn native loading. No user media."""
import hashlib
import json
import struct
import subprocess


def run(Session, out, image, success, basic, env, app, selected=None, trace=False, pulse_tool=None):
    source = image.read_bytes()
    original_hash = hashlib.sha256(source).hexdigest()

    def fixture(name, gap, blank_tracks):
        data = bytearray(688)
        for ht in range(168):
            offset, = struct.unpack_from('<I', source, 16 + ht * 4)
            if not offset:
                continue
            duration, count, start = struct.unpack_from('<III', source, offset + 4)
            edges = []
            time = 0
            for word, in struct.iter_unpack('>H', source[offset + start:offset + start + count * 2]):
                time += word or 65536
                if word:
                    edges.append(time)
            struct.pack_into('<I', data, 16 + ht * 4, len(data))
            track = bytearray(b'TRK' + bytes([ht]) + bytes(24))
            previous = 0
            second = duration - gap if 'bounded' in name else duration
            for rev, (origin, length) in enumerate([(0, gap), (gap, second)]):
                samples = []
                for edge in edges:
                    if name.startswith('control-bounded'):
                        if (edge <= gap) != (rev == 0):
                            continue
                        absolute = edge
                    else:
                        if (rev == 0 and ht in blank_tracks) or edge > length:
                            continue
                        absolute = origin + edge
                    delta = absolute - previous
                    while delta > 65535:
                        samples.append(0)
                        delta -= 65536
                    assert delta > 0
                    samples.append(delta)
                    previous = absolute
                struct.pack_into('<III', track, 4 + rev * 12, length, len(samples), len(track))
                track += struct.pack('>' + 'H' * len(samples), *samples)
            data += track
        data[:16] = source[:16]
        data[5] = 2
        data[8] |= 0x80
        struct.pack_into('<I', data, 12, sum(data[16:]) & 0xffffffff)
        path = out / (name + '.scp')
        path.write_bytes(data)
        path.chmod(0o444)
        return path

    # Run KERNAL LOAD, preserve its A/carry, and read the drive's DOS channel.
    # Stop at a checkpoint instead of DebugCart so the result and live native
    # state can be inspected even when the ROM rejects the medium.
    code = bytearray(bytes.fromhex('a907a280a0c120bdffa901a208a00120baffa900a200a00020d5ff'))
    code += bytes.fromhex('8d00c2086829018d01c2')
    code += bytes.fromhex('a900a200a00020bdffa90fa208a00f20baff20c0ffa20f20c6ffa000')
    loop = len(code)
    code += bytes.fromhex('20cfff9910c2c8c90df000c028d000')
    code[-1] = (loop - len(code)) & 255
    done = 0xc000 + len(code)
    code[loop + 10] = len(code) - (loop + 11)
    code += bytes([0x4c, done & 255, done >> 8])
    assert len(code) < 0x180
    ram = bytearray(basic + bytes.fromhex('4c0d08'))
    ram += bytes(0xc000 - 0x0801 - len(ram)) + code
    ram += bytes(0xc180 - 0x0801 - len(ram)) + b'SUCCESS'
    program = out / 'diagnostic.prg'
    program.write_bytes(b'\x01\x08' + ram)

    def memory(session, start, length, space=0):
        rc, raw = session.command(0x01, struct.pack('<BHHBH', 0, start, start + length - 1, space, 0))
        assert rc == 0 and len(raw) == length + 2, (rc, len(raw))
        return raw[2:]

    results = {}
    cases = [('control', 8000000, set()),
             ('blank-all-196ms', 7840000, set(range(84))),
             ('blank-all-200ms', 8000000, set(range(84))),
             ('blank-directory-200ms', 8000000, {34}),
             ('blank-data-200ms', 8000000, {32}),
             ('blank-all-20ms', 800000, set(range(84))),
             ('blank-all-10ms', 400000, set(range(84))),
             ('blank-all-5ms', 200000, set(range(84))),
             ('blank-all-1ms', 40000, set(range(84))),
             ('control-bounded', 800000, set()),
             ('blank-bounded-5ms', 200000, set(range(84))),
             ('blank-bounded-10ms', 400000, set(range(84))),
             ('blank-bounded-20ms', 800000, set(range(84)))]
    if pulse_tool:
        cases += [(name + '-p64', gap, tracks) for name, gap, tracks in list(cases) if 'bounded' in name]
    assert not selected or set(selected) <= {name for name, _, _ in cases}
    for name, gap, tracks in cases:
        if selected and name not in selected:
            continue
        disk = fixture(name, gap, tracks)
        pristine = name.endswith('-p64')
        if pristine:
            target = disk.with_suffix('.p64')
            subprocess.run([str(pulse_tool.resolve()), str(disk), str(target)], check=True, env=env, timeout=30)
            target.chmod(0o444)
            disk = target
        digest = hashlib.sha256(disk.read_bytes()).hexdigest()
        session = Session(name, disk=disk, program=program, pristine=pristine)
        try:
            trace_rows = []
            if trace:
                rom = (app / 'DRIVES/dos1541ii-251968-03.bin').read_bytes()
                assert rom[0x3556:0x356e] == bytes.fromhex('a9d08d0518a9032c051810f12c001c30f6ad011cb8a00060')
                for pc in [0xf553, 0xf567]:
                    rc, checkpoint = session.command(0x12, struct.pack('<HHBBBBB', pc, pc, 1, 1, 4, 0, 1))
                    assert rc == 0
                    if pc == 0xf567:
                        success_checkpoint = struct.unpack_from('<I', checkpoint)[0]
            rc, _ = session.command(0x12, struct.pack('<HHBBBBB', done, done, 1, 1, 4, 0, 0))
            assert rc == 0
            rc, _ = session.command(0x32, bytes([0, 1, 0, 3, 3, 0, 0xc0]))
            assert rc == 0
            session.events = []
            session.checkpoint_hits = []
            rc, _ = session.command(0xaa)
            assert rc == 0
            while True:
                pc = session.wait_stop()
                if pc == done:
                    break
                hits = [hex(struct.unpack_from('<H', hit, 5)[0])
                        for hit in session.checkpoint_hits if hit[22] == 1]
                session.checkpoint_hits = []
                assert trace and hits, hex(pc)
                raw = session.save('trace.vsf', 0).read_bytes()
                flux = raw.index(b'FLUX8:0' + bytes([0])) + 22
                drive = raw.index(b'DRIVE8' + bytes([0])) + 22
                turn, phase, duration, elapsed = struct.unpack_from('<QQQQ', raw, flux + 21)
                trace_rows.append({'hit_addresses': hits, 'turn': turn, 'phase': phase, 'duration': duration,
                                   'elapsed': elapsed, 'halftrack': raw[flux + 53],
                                   'shift_register': struct.unpack_from('<H', raw, drive + 64)[0],
                                   'attach_clock': struct.unpack_from('<Q', raw, drive + 6)[0]})
                (out / (name + '-sync-trace.json')).write_text(json.dumps(trace_rows, indent=2) + '\n')
                assert len(trace_rows) < 2000
                if len(trace_rows) == 8:
                    rc, _ = session.command(0x15, struct.pack('<IB', success_checkpoint, 0))
                    assert rc == 0
                rc, _ = session.command(0xaa)
                assert rc == 0
            status = memory(session, 0xc200, 56)
            payload = memory(session, 0x0801, len(success.read_bytes()) - 2)
            drive_ram = memory(session, 0, 2048, 1)
            (session.directory / 'drive-ram.bin').write_bytes(drive_ram)
            snapshot = session.save('result.vsf', 0).read_bytes()
            results[name] = {'kernal_a': status[0], 'kernal_carry': status[1],
                             'dos_status': status[16:].split(b'\r')[0].decode('ascii', 'replace'),
                             'exact_payload': payload == success.read_bytes()[2:],
                             'pristine_upstream_p64': pristine}
            if not pristine:
                at = snapshot.index(b'FLUX8:0' + bytes([0])) + 22
                turn, phase, duration, elapsed = struct.unpack_from('<QQQQ', snapshot, at + 21)
                results[name].update(cursor_turn=turn, cursor_phase=phase, cursor_duration=duration,
                                     cursor_elapsed=elapsed, halftrack=snapshot[at + 53])
            assert not name.startswith('control') or (status[1] == 0 and results[name]['exact_payload'])
            print(name, json.dumps(results[name]), flush=True)
        finally:
            session.close()
        assert hashlib.sha256(disk.read_bytes()).hexdigest() == digest
        (out / 'blank-turn-diagnostics.json').write_text(json.dumps(results, indent=2) + '\n')
    assert hashlib.sha256(image.read_bytes()).hexdigest() == original_hash
    comparisons = {}
    for name, result in results.items():
        if name.endswith('-p64') or name + '-p64' not in results:
            continue
        reference = results[name + '-p64']
        comparisons[name] = all(result[key] == reference[key]
                                for key in ['kernal_carry', 'dos_status', 'exact_payload'])
        assert comparisons[name], (result, reference)
    (out / 'blank-turn-comparisons.json').write_text(json.dumps(comparisons, indent=2) + '\n')
