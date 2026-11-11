#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read KMD recovery events from a SWETW001 raw ProcessTrace export.

Input is the retained binary export, not the original ETL. Output is JSONL;
counts and input identity go to stderr. Absence of an event is not success.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import uuid

PROVIDER = uuid.UUID('c27c1f9b-62e8-4e21-b29b-6bb2d585c6d0').bytes_le
HEADER = struct.Struct('<8sQQII')
EVENT = struct.Struct('<Q16sIIHHHHBBB5x')
PAYLOAD = struct.Struct('<6IQ10I')
SOURCES = ('unspecified', 'submit_gate', 'submit', 'reject_drain', 'preempt',
           'worker', 'legacy_submit')


def decode(data):
    if len(data) < HEADER.size:
        raise ValueError('truncated export header')
    magic, expected, last, seconds, header_size = HEADER.unpack_from(data)
    if magic != b'SWETW001' or header_size != EVENT.size:
        raise ValueError('unsupported raw export format')
    offset, total, records = HEADER.size, 0, []
    while offset < len(data):
        if len(data) - offset < EVENT.size:
            raise ValueError('truncated event header')
        time, provider, pid, tid, cpu, event_id, flags, size, opcode, version, level = EVENT.unpack_from(data, offset)
        offset += EVENT.size
        if size > len(data) - offset:
            raise ValueError('truncated event payload')
        payload = data[offset:offset + size]
        offset += size
        total += 1
        if provider != PROVIDER:
            continue
        if (event_id, version, size) != (1, 1, PAYLOAD.size):
            raise ValueError('unsupported KMD recovery event schema')
        values = PAYLOAD.unpack(payload)
        if values[0] != 1:
            raise ValueError('unsupported recovery payload version')
        source = values[1]
        records.append({
            'event': 'FirstRecoveryRequest', 'time': time, 'pid': pid,
            'tid': tid, 'cpu': cpu, 'source': source,
            'source_name': SOURCES[source] if source < len(SOURCES) else 'unknown',
            'adapter_luid': f'{values[3]:08x}{values[2]:08x}',
            'status': f'0x{values[4]:08x}', 'flags': f'0x{values[5]:08x}',
            'context': f'0x{values[6]:016x}', 'dma_bytes': values[7],
            'private_bytes': values[8], 'umd_private_bytes': values[9],
            'submission_fence': values[10], 'last_completed_fence': values[11],
            'graphics_generation': values[12],
            'cached_fifo': f'0x{values[13]:08x}',
            'cached_mmu': f'0x{values[14]:08x}',
            'cached_pbdma': f'0x{values[15]:08x}',
            'cached_pbdma1': f'0x{values[16]:08x}',
        })
    if total != expected:
        raise ValueError(f'export event count differs: {total}/{expected}')
    return records, {'export_records': total, 'recovery_records': len(records),
                     'last_timestamp': last, 'requested_seconds': seconds,
                     'sha256': hashlib.sha256(data).hexdigest()}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    args = parser.parse_args()
    records, summary = decode(args.binary.read_bytes())
    for record in records:
        print(json.dumps(record, separators=(',', ':')))
    print(json.dumps(summary), file=sys.stderr)
