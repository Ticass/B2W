import struct
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import ipakfix


def block(offset, size, fill):
    # one uncompressed command filling the block
    header = struct.pack('<II', 1 << 24 | offset, size - 128) + bytes(120)
    return header + bytes([fill]) * (size - 128)


def ipak(blocks):
    data = b''.join(blocks)
    index_offset = 0x8000 + len(data)
    head = b'KAPI' + struct.pack('<III', 0x50000, index_offset + 16, 2)
    head += struct.pack('<4I', 2, 0x8000, len(data), 1) + struct.pack('<4I', 1, index_offset, 16, 1)
    entry = struct.pack('<4I', 0x1234, 0x5678, 0, len(data))
    return head + bytes(0x8000 - len(head)) + data + entry


class IpakRepairTests(unittest.TestCase):
    def test_a_block_crossing_a_window_end_moves_to_the_next_window(self):
        # School mod.ff ipak: after a short block the mod tools linker restarted
        # its window, so the next 0x40000 block crossed the streamer's window end
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'mod.ipak'
            path.write_bytes(ipak([block(0, 0x8000, 1), block(0x7F80, 0x40000, 2)]))
            self.assertIsNotNone(ipakfix.first_violation(bytearray(path.read_bytes())))
            self.assertEqual(ipakfix.repair(path), [0x38000])
            data = bytearray(path.read_bytes())
            self.assertIsNone(ipakfix.first_violation(data))
            self.assertEqual(struct.unpack_from('<I', data, 8)[0], len(data))
            index_offset, _, _ = ipakfix._section(data, ipakfix.INDEX)
            self.assertEqual(index_offset, 0x8000 + 0x48000 + 0x38000)
            self.assertEqual(struct.unpack_from('<4I', data, index_offset), (0x1234, 0x5678, 0, 0x80000))
            # the skip block, then the moved block's data intact at the window start
            self.assertEqual(struct.unpack_from('<II', data, 0x10000), (1 << 24, ipakfix.SKIP << 24 | 0x38000 - 128))
            self.assertEqual(data[0x48000:0x48004], struct.pack('<I', 1 << 24 | 0x7F80))
            self.assertEqual(set(data[0x48080:0x88000]), {2})
            self.assertEqual(ipakfix.repair(path), [])


if __name__ == '__main__':
    unittest.main()
