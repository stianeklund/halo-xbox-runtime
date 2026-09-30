#!/usr/bin/env python3
"""Read audio diagnostics from ordinary RAM on a running standalone build.

  python3 tools/audio_status.py build-seed/halo.map --host 127.0.0.1

Use the map matching the deployed build. Never access AC97 MMIO through XBDM:
its bytewise getmem implementation can crash xemu on unsupported byte reads.
"""
import argparse
import json
import re
import struct

from symbolize import MAP_RE, rdcp

TRANSPORT = ("irq_callbacks", "rendered_buffers", "queued_buffers", "pcm_registers",
             "spdif_registers", "underruns", "retired_pcm_descriptors", "failed_waits")
MIXER = ("submitted_packets", "rejected_packets", "completed_packets", "aborted_packets",
         "render_calls", "nonzero_frames", "peak_sample", "game_queue_calls")
APU = ("ready", "created_voices", "destroyed_voices", "queued_buffers",
       "completed_buffers", "worker_polls", "stop_timeouts", "driver_errors")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("map")
    parser.add_argument("--host", default="127.0.0.1")
    args = parser.parse_args()
    symbols = {}
    with open(args.map) as source:
        for line in source:
            match = MAP_RE.match(line)
            if match:
                symbols[match[1]] = int(match[2], 16)

    def read(address, size):
        if address >= 0x80000000:
            raise ValueError("audio diagnostics must read ordinary title RAM")
        response = rdcp(args.host, f"getmem addr=0x{address:x} length=0x{size:x}")
        data = "".join(line.strip() for line in response.splitlines()[1:]
                       if re.fullmatch(r"[0-9A-Fa-f?]+", line.strip()))
        if "?" in data or len(data) != size * 2:
            raise RuntimeError(f"could not read audio diagnostics at 0x{address:x}: {response}")
        return bytes.fromhex(data)

    result = {}
    for name, fields in (("transport", TRANSPORT), ("apu", APU), ("mixer", MIXER)):
        address = symbols.get(f"_halo_audio_{name}_stats")
        if address is None:
            continue
        result[name] = dict(zip(fields, struct.unpack("<8I", read(address, 32))))
    for name in (("pcm_registers", "spdif_registers") if "transport" in result else ()):
        raw = result["transport"][name]
        result["transport"][name] = {"civ": raw & 31, "lvi": (raw >> 8) & 31,
                                      "status": f"0x{raw >> 16:04x}"}
    channels = []
    count = int.from_bytes(read(0x4fdfc4, 2), "little")
    for index in range(min(count, 256)):
        data = read(0x4fdfc8 + index * 0x74, 0x74)
        state = struct.unpack_from("<h", data)[0]
        pending = struct.unpack_from("<h", data, 8)[0]
        if state or pending:
            channels.append({"index": index, "state": state, "pending": pending,
                             "flags": f"0x{struct.unpack_from('<H', data, 0x38)[0]:x}"})
    result["active_channels"] = channels
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
