"""Exercise production image probing/ownership with recorded file-open modes."""
import ctypes as C
import hashlib
import os
from pathlib import Path
import struct
import sys
import unittest

library = Path(sys.argv.pop(1)).resolve()
out = Path(sys.argv.pop(1)).resolve()
if os.name == 'nt':
    dll_dir = os.add_dll_directory(str(library.parent))
lib = C.CDLL(str(library))


def api(name, result=C.c_int, *args):
    f = getattr(lib, name)
    f.restype = result
    f.argtypes = list(args)
    return f


reset = api('lifecycle_reset', None)
metric = api('lifecycle_metric', C.c_int, C.c_int)
open_image = api('lifecycle_open', C.c_int, C.c_char_p, C.c_int, C.c_int)
close = api('lifecycle_close')
destroy = api('lifecycle_destroy', None)
kind = api('lifecycle_type')
readonly = api('lifecycle_readonly')
capture = api('lifecycle_capture')
read_image = api('lifecycle_read_image')
write = api('lifecycle_write', C.c_int, C.c_int)
deny_writable = api('lifecycle_fail_writable', None, C.c_int)
core_reset = api('native_test_reset', None)
core_live = api('native_test_live')
fail_alloc = api('native_test_fail_alloc', None, C.c_int)
fail_read = api('native_test_fail_read', None, C.c_int)
fail_seek = api('native_test_fail_seek', None, C.c_int)


def scp(flags=1):
    data = bytearray(688)
    data[:16] = struct.pack('<3s9BI', b'SCP', 0x17, 0, 1, 0, 0, flags, 0, 1, 0, 0)
    struct.pack_into('<I', data, 16, 688)
    data += b'TRK\0' + struct.pack('<III', 8000000, 3, 16)
    data += struct.pack('>HHH', 80, 160, 240)
    struct.pack_into('<I', data, 12, 0 if flags & 16 else sum(data[16:]))
    return data


class LifecycleTests(unittest.TestCase):
    def setUp(self):
        reset()
        core_reset()
        self.assertEqual(metric(3), 0)
        self.assertEqual(metric(5), 0)
        self.assertEqual(core_live(), 0)
        self.files = []

    def tearDown(self):
        core_reset()
        destroy()
        self.assertEqual(metric(3), 0, 'file handle leaked')
        self.assertEqual(metric(5), 0, 'fsimage allocation leaked')
        self.assertEqual(core_live(), 0, 'capture allocation leaked')
        for p, digest in self.files:
            self.assertEqual(hashlib.sha256(p.read_bytes()).digest(), digest)

    def fixture(self, data=None, suffix='.scp', protected=False):
        p = out / (self.id().split('.')[-1] + suffix)
        if p.exists():
            p.chmod(0o666)
        p.write_bytes(scp() if data is None else data)
        if protected:
            p.chmod(0o444)
        self.files.append((p, hashlib.sha256(p.read_bytes()).digest()))
        return str(p).encode('utf-8')

    def test_writable_attachment_forced_readonly(self):
        self.assertEqual(open_image(self.fixture(), 0, 0), 0)
        self.assertEqual((kind(), readonly(), capture(), read_image()), (201, 1, 1, 0))
        self.assertEqual((metric(0), metric(1), metric(3)), (1, 0, 1))

    def test_readonly_attachment(self):
        self.assertEqual(open_image(self.fixture(protected=True), 1, 0), 0)
        self.assertEqual((capture(), metric(1)), (1, 0))

    def test_signature_overrides_extension(self):
        self.assertEqual(open_image(self.fixture(suffix='.d64'), 0, 0), 0)
        self.assertEqual((kind(), metric(1)), (201, 0))

    def test_writable_mode_scp_still_protected(self):
        self.assertEqual(open_image(self.fixture(scp(flags=17)), 0, 0), 0)
        self.assertEqual((readonly(), metric(1)), (1, 0))

    def test_probe_disposes_capture(self):
        self.assertEqual(open_image(self.fixture(), 1, 1), 0)
        self.assertEqual((kind(), capture(), metric(3), core_live()), (201, 0, 0, 0))

    def test_close_disposes_capture(self):
        self.assertEqual(open_image(self.fixture(), 0, 0), 0)
        self.assertEqual(close(), 0)
        self.assertEqual((capture(), core_live(), metric(3)), (0, 0, 0))
        self.assertLess(close(), 0)
        self.assertEqual(metric(2), 1)

    def test_destroy_without_close_and_repeat(self):
        self.assertEqual(open_image(self.fixture(), 0, 0), 0)
        destroy()
        destroy()
        self.assertEqual((metric(2), metric(5), core_live()), (1, 0, 0))

    def test_writes_rejected_even_if_readonly_flag_cleared(self):
        self.assertEqual(open_image(self.fixture(), 0, 0), 0)
        for which in range(4):
            self.assertEqual(write(which), -1)
        self.assertEqual(metric(4), 0, 'SCP reached a legacy format writer')

    def test_malformed_signature_does_not_fall_through_to_d64(self):
        data = bytearray(174848)
        data[:3] = b'SCP'
        self.assertEqual(open_image(self.fixture(data), 0, 0), -1)
        self.assertEqual((kind(), capture(), metric(1), metric(3)), (0, 0, 0, 0))

    def test_malformed_probe_no_false_success(self):
        data = scp()
        data[-1] ^= 1
        self.assertEqual(open_image(self.fixture(data), 1, 1), -1)
        self.assertEqual((capture(), metric(3)), (0, 0))

    def test_recognized_short_header_no_writable_open(self):
        self.assertEqual(open_image(self.fixture(b'SCP'), 0, 0), -1)
        self.assertEqual((metric(1), metric(3)), (0, 0))

    def test_ordinary_d64_writable_policy_retained(self):
        self.assertEqual(open_image(self.fixture(bytes(174848), '.d64'), 0, 0), 0)
        self.assertEqual((kind(), readonly(), capture(), metric(1)), (1541, 0, 0, 1))

    def test_ordinary_d64_readonly_policy_retained(self):
        self.assertEqual(open_image(self.fixture(bytes(174848), '.d64'), 1, 0), 0)
        self.assertEqual((kind(), readonly(), metric(1)), (1541, 1, 0))

    def test_ordinary_d64_readonly_fallback(self):
        deny_writable(1)
        self.assertEqual(open_image(self.fixture(bytes(174848), '.d64'), 0, 0), 0)
        self.assertEqual((kind(), readonly(), metric(1)), (1541, 1, 1))

    def test_unknown_image_closes_handle(self):
        self.assertEqual(open_image(self.fixture(b'not an image'), 0, 0), -1)
        self.assertEqual(metric(3), 0)

    def test_capture_allocation_failures_cleanup(self):
        path = self.fixture()
        succeeded = False
        for n in range(30):
            fail_alloc(n)
            rc = open_image(path, 0, 0)
            core_reset()
            if rc == 0:
                succeeded = True
            self.assertEqual(metric(1), 0)
            destroy()
            self.assertEqual((metric(3), metric(5), core_live()), (0, 0, 0))
            if succeeded:
                break
        self.assertTrue(succeeded)

    def test_read_errors_after_signature_do_not_fall_through(self):
        path = self.fixture()
        for n in range(1, 6):
            fail_read(n)
            self.assertEqual(open_image(path, 0, 0), -1)
            core_reset()
            destroy()
            self.assertEqual((metric(1), metric(3), core_live()), (0, 0, 0))

    def test_seek_error_closes_handle(self):
        fail_seek(0)
        self.assertEqual(open_image(self.fixture(), 0, 0), -1)
        self.assertEqual((metric(1), metric(3)), (0, 0))


if __name__ == '__main__':
    unittest.main(verbosity=2)
