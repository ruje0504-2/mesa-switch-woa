# SPDX-License-Identifier: MIT
import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location(
    'decode_recovery', Path(__file__).resolve().parents[1] / 'tools/decode_recovery.py')
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)

# Fixed wire bytes, independent of the decoder's field packing. Match the
# 72-byte version-1 C record, including a negative NTSTATUS and 64-bit handles.
PAYLOAD = bytes.fromhex('''
    01000000 04000000 88776655 44332211 170000c0 09000000
    8070605040302010 40000000 00010000 20000000 6c000000
    6b000000 03000000 01000000 02000000 04000000 08000000
''')


def export(payload=PAYLOAD, version=1, count=1):
    return (struct.pack('<8sQQII', b'SWETW001', count, 1337, 12, 48) +
            struct.pack('<Q16sIIHHHHBBB5x', 1337, decoder.PROVIDER,
                        4, 6020, 2, 1, 0x40, len(payload), 0, version, 2) + payload)


class RecoveryDecode(unittest.TestCase):
    def test_wire_identity(self):
        records, summary = decoder.decode(export())
        self.assertEqual(summary['export_records'], 1)
        self.assertEqual(summary['recovery_records'], 1)
        r = records[0]
        self.assertEqual((r['pid'], r['tid'], r['cpu'], r['time']), (4, 6020, 2, 1337))
        self.assertEqual(r['source_name'], 'preempt')
        self.assertEqual(r['adapter_luid'], '1122334455667788')
        self.assertEqual(r['status'], '0xc0000017')
        self.assertEqual(r['context'], '0x1020304050607080')
        self.assertEqual([r[k] for k in ('dma_bytes', 'private_bytes', 'umd_private_bytes',
                         'submission_fence', 'last_completed_fence', 'graphics_generation')],
                         [64, 256, 32, 108, 107, 3])
        self.assertEqual([r[k] for k in ('cached_fifo', 'cached_mmu', 'cached_pbdma', 'cached_pbdma1')],
                         ['0x00000001', '0x00000002', '0x00000004', '0x00000008'])

    def test_incomplete_or_incompatible_export(self):
        for data in (export()[:31], export()[:79], export()[:-1],
                     export(count=2), export(version=2), export(PAYLOAD[:-1]),
                     export(b'\x02' + PAYLOAD[1:])):
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                decoder.decode(data)

    def test_other_provider_is_not_a_recovery(self):
        data = bytearray(export())
        data[40:56] = bytes(16)
        records, summary = decoder.decode(data)
        self.assertEqual(records, [])
        self.assertEqual(summary['export_records'], 1)


if __name__ == '__main__':
    unittest.main()
