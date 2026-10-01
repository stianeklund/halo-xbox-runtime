#!/usr/bin/env python3
"""Exercise the linked 32-bit audio adapter/mixer with simulated nxdk transport.

Build the audio-enabled target first, then run:
  python3 tools/test_audio.py build-seed
Requires pefile and unicorn. This does not emulate the AC97 hardware.
"""
import re
import struct
import sys
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_MEM_READ
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ESP


class AudioImage:
    def __init__(self, build):
        pe = pefile.PE(str(build / "halo.exe"))
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        base = pe.OPTIONAL_HEADER.ImageBase
        self.uc.mem_map(base, (pe.OPTIONAL_HEADER.SizeOfImage + 4095) & ~4095)
        self.uc.mem_write(base, pe.get_memory_mapped_image())
        self.uc.mem_map(0x10000000, 0x1000000)
        self.uc.mem_map(0x20000000, 0x100000)
        self.uc.mem_map(0x30000000, 0x1000)
        self.uc.mem_write(0x30000000, b"\xc3")
        self.next_memory = 0x10000000
        self.symbols = {}
        for line in (build / "halo.map").read_text().splitlines():
            match = re.match(r"\s+[0-9a-f]+:[0-9a-f]+\s+(\S+)\s+([0-9a-f]+)\s", line, re.I)
            if match:
                self.symbols[match[1]] = int(match[2], 16)
        self.callbacks = []
        self.freed = []
        self.events = []
        self.locked = False
        self.hooks = {
            self.symbols["_standalone_audio_init"]: self.initialize,
            self.symbols["_standalone_audio_lock"]: self.lock,
            self.symbols["_standalone_audio_unlock"]: self.unlock,
            self.symbols["_calloc"]: self.calloc,
            self.symbols["_free"]: self.free,
            self.symbols["_SetEvent"]: self.event,
        }
        self.callback = 0x30000010
        self.hooks[self.callback] = self.completed
        self.uc.hook_add(UC_HOOK_CODE, self.hook)

    def alloc(self, data):
        address = self.next_memory
        self.next_memory += (len(data) + 15) & ~15
        assert self.next_memory < 0x11000000
        self.uc.mem_write(address, bytes(data))
        return address

    def words(self, address, count=1):
        return struct.unpack("<" + "I" * count, self.uc.mem_read(address, count * 4))

    def args(self, count):
        return self.words(self.uc.reg_read(UC_X86_REG_ESP) + 4, count)

    def return_from_hook(self, value=0, pops=0):
        from unicorn.x86_const import UC_X86_REG_EIP
        esp = self.uc.reg_read(UC_X86_REG_ESP)
        self.uc.reg_write(UC_X86_REG_EAX, value)
        self.uc.reg_write(UC_X86_REG_EIP, self.words(esp)[0])
        self.uc.reg_write(UC_X86_REG_ESP, esp + 4 + pops)

    def initialize(self):
        self.return_from_hook(1)

    def lock(self):
        assert not self.locked, "recursive backend lock"
        self.locked = True
        self.return_from_hook()

    def unlock(self):
        assert self.locked, "unlock without lock"
        self.locked = False
        self.return_from_hook()

    def calloc(self):
        count, size = self.args(2)
        self.return_from_hook(self.alloc(bytes(count * size)))

    def free(self):
        address, = self.args(1)
        assert address not in self.freed, "double release"
        self.freed.append(address)
        self.return_from_hook()

    def event(self):
        self.events.append(self.args(1)[0])
        self.return_from_hook(1)

    def completed(self):
        assert not self.locked, "game callback under backend lock"
        self.callbacks.append(self.args(3))
        self.return_from_hook(pops=12)

    def hook(self, uc, address, size, _):
        handler = self.hooks.get(address)
        if handler:
            handler()

    def call(self, name_or_address, *args, stdcall=None):
        address = self.symbols[name_or_address] if isinstance(name_or_address, str) else name_or_address
        if stdcall is None:
            stdcall = isinstance(name_or_address, str) and "@" in name_or_address
        esp = 0x200ff000
        self.uc.mem_write(esp, struct.pack("<" + "I" * (len(args) + 1), 0x30000000,
                                         *(a & 0xffffffff for a in args)))
        self.uc.reg_write(UC_X86_REG_ESP, esp)
        self.uc.emu_start(address, 0x30000000, count=2000000)
        expected = esp + 4 + (len(args) * 4 if stdcall else 0)
        assert self.uc.reg_read(UC_X86_REG_ESP) == expected, f"stack imbalance in {name_or_address}"
        assert not self.locked
        return self.uc.reg_read(UC_X86_REG_EAX)

    def format(self, channels=1, rate=48000, bits=16, adpcm=False):
        align = 36 * channels if adpcm else channels * bits // 8
        return self.alloc(struct.pack("<HHIIHHHH", 0x69 if adpcm else 1, channels,
                                      rate, rate * align, align, 4 if adpcm else bits,
                                      2 if adpcm else 0, 64 if adpcm else 0))

    def stream(self, format_address=None, callback=True, capacity=4):
        if format_address is None:
            format_address = self.format()
        desc = self.alloc(struct.pack("<IIIIII", 0, capacity, format_address,
                                      self.callback if callback else 0, 42, 0))
        out = self.alloc(bytes(4))
        assert self.call("_DirectSoundCreateStream@12", desc, out, 0) == 0
        return self.words(out)[0]

    def method(self, stream, slot, *args):
        vtable, = self.words(stream)
        address = self.words(vtable + slot * 4)[0]
        return self.call(address, stream, *args, stdcall=True)

    def submit(self, stream, data, context=99):
        payload = self.alloc(data)
        done = self.alloc(bytes(4))
        status = self.alloc(bytes(4))
        packet = self.alloc(struct.pack("<IIIIII", payload, len(data), done, status, context, 0))
        result = self.method(stream, 4, packet, 0)
        return result, done, status

    def render(self, frames):
        out = self.alloc(bytes(frames * 4))
        self.call("_standalone_audio_render", out, frames)
        return struct.unpack("<" + "h" * (frames * 2), self.uc.mem_read(out, frames * 4))

    def service(self):
        self.call("_DirectSoundDoWork")

    def release(self, stream):
        assert self.method(stream, 1) == 0
        self.service()
        assert stream in self.freed

    def initialize_channel(self, flags, index):
        # The game's lift can inline channel_get and checks the global count.
        channel = 0x4fdfc8 + index * 0x74
        self.uc.mem_write(channel, bytes(0x74))
        self.uc.mem_write(0x4fdfc4, struct.pack("<h", index + 1))
        self.uc.mem_write(0x4fdbc0, b"\x00")
        self.uc.mem_write(0x505488, struct.pack("<f", 1.0))  # pause gain
        assert self.call("_dsound_initialize_channel", flags, index) & 255 == 1
        return channel, self.words(channel + 0x70)[0]

    def channel_gain(self, index, gain):
        properties = self.alloc(struct.pack("<8f", 1, 1, 1, gain, 0, 0, 0, 0))
        self.call("_sound_dsound_update_channel_properties", properties, index, 0)


def pcm(*samples):
    return struct.pack("<" + "h" * len(samples), *samples)


def output_samples(samples):
    # Reference DirectSound headroom for ordinary 2D voices.
    gain = int(32768 * 10 ** (-6 / 20))
    return tuple(max(-32768, min(32767, int(sample * gain / 32768)))
                 for sample in samples)


def test_output_clipping(build):
    image = AudioImage(build)
    samples = (32767, -32768, 60000, -60000, 200000, -200000,
               2147483647, -2147483648, 0, 0)
    mixed = image.alloc(struct.pack("<10i", *samples))
    output = image.alloc(bytes(20))
    image.call("_audio_mix_clip", mixed, output, 5)
    assert struct.unpack("<10h", image.uc.mem_read(output, 20)) == (
        32767, -32768, 32767, -32768, 32767, -32768, 32767, -32768, 0, 0)


def test_stream_lifecycle(build):
    image = AudioImage(build)
    stream = image.stream()
    # The global stream creation API must work without DirectSoundCreate.
    result, done, status = image.submit(stream, pcm(1200, -1200, 32767, -32768))
    assert result == 0 and image.words(status)[0] == 0x8000000a
    inner, = image.words(stream + 0x24)
    assert image.words(inner + 8)[0] == 2
    assert image.render(4) == output_samples((1200, 1200, -1200, -1200, 32767, 32767, -32768, -32768))
    assert image.words(inner + 8)[0] == 0
    assert image.callbacks == [] and image.words(done)[0] == 0
    image.service()
    assert image.callbacks == [(42, 99, 0)]
    assert image.words(done)[0] == 8 and image.words(status)[0] == 0
    assert image.method(stream, 0) == 2
    assert image.method(stream, 1) == 1
    image.release(stream)


def test_capacity_flush_pause(build):
    image = AudioImage(build)
    stream = image.stream()
    packets = [image.submit(stream, pcm(100, 200), context=i) for i in range(4)]
    assert all(p[0] == 0 for p in packets)
    assert image.submit(stream, pcm(999))[0] == 0x80070057
    status = image.alloc(bytes(4))
    assert image.method(stream, 3, status) == 0 and not image.words(status)[0] & 1
    assert image.call("_IDirectSoundStream_Pause@8", stream, 1) == 0
    assert image.render(4) == (0,) * 8
    assert image.call("_IDirectSoundStream_Pause@8", stream, 0) == 0
    assert image.render(1) == output_samples((100, 100))
    # Thiscall inner slot 4 is the stop path used by Halo (ECX=this, RET 8).
    from unicorn.x86_const import UC_X86_REG_ECX
    inner, = image.words(stream + 0x24)
    inner_vtable, = image.words(inner)
    image.uc.reg_write(UC_X86_REG_ECX, inner)
    image.call(image.words(inner_vtable + 16)[0], 0, 0, stdcall=True)
    assert image.callbacks == [(42, i, 0x80004004) for i in range(4)]
    assert image.render(2) == (0,) * 4
    image.service()
    assert image.callbacks == [(42, i, 0x80004004) for i in range(4)]
    assert image.words(packets[0][1])[0] == 2
    assert image.submit(stream, pcm(321))[0] == 0
    assert image.render(1) == output_samples((321, 321))
    image.service()
    image.release(stream)


def test_resampling_volume_mix(build):
    image = AudioImage(build)
    stream = image.stream(image.format(rate=24000))
    assert image.submit(stream, pcm(1000, 2000))[0] == 0
    assert image.render(4) == output_samples((1000, 1000, 1000, 1000, 2000, 2000, 2000, 2000))
    image.service()
    assert image.call("_IDirectSoundStream_SetFrequency@8", stream, 48000) == 0
    assert image.call("_IDirectSoundStream_SetVolume@8", stream, -600) == 0
    assert image.submit(stream, pcm(10000))[0] == 0
    sample = image.render(1)[0]
    assert 2511 <= sample <= 2512
    image.service()
    assert image.call("_IDirectSoundStream_SetVolume@8", stream, -10000) == 0
    assert image.submit(stream, pcm(30000))[0] == 0
    assert image.render(1) == (0, 0)
    image.service()
    image.release(stream)
    # Two loud voices fit within output headroom without premature clipping.
    a, b = image.stream(), image.stream()
    image.submit(a, pcm(30000, -30000))
    image.submit(b, pcm(30000, -30000))
    # Each voice applies and truncates its own headroom before summation.
    assert image.render(2) == tuple(2 * value for value in
                                   output_samples((30000, 30000, -30000, -30000)))
    image.service()
    image.release(a)
    image.release(b)
    c = image.stream(image.format(bits=8))
    image.submit(c, bytes([0, 128, 255]))
    assert image.render(3) == output_samples((-32768, -32768, 0, 0, 32512, 32512))
    image.service()
    image.release(c)
    # Resampler overshoot must carry through several tiny packets.
    fast = image.stream(image.format(rate=192000))
    for i in range(4): image.submit(fast, pcm(i + 1), i)
    assert image.render(1) == output_samples((1, 1))
    image.service()
    assert [c[1] for c in image.callbacks[-4:]] == list(range(4))
    image.release(fast)


def test_adpcm(build):
    image = AudioImage(build)
    mono = image.stream(image.format(adpcm=True))
    block = struct.pack("<hBB", 100, 0, 0) + b"\x11" * 31 + b"\xf1"
    assert image.submit(mono, block)[0] == 0
    rendered = image.render(64)
    assert rendered[::2] == output_samples(range(100, 164)) and rendered[1::2] == rendered[::2]
    image.service()
    image.release(mono)
    stereo = image.stream(image.format(channels=2, adpcm=True))
    block = struct.pack("<hBBhBB", 1000, 0, 0, -1000, 0, 0)
    block += (b"\x11" * 4 + b"\x99" * 4) * 8
    assert image.submit(stereo, block + block)[0] == 0
    rendered = image.render(128)
    assert rendered[::2] == output_samples(range(1000, 1064)) * 2
    assert rendered[1::2] == output_samples(range(-1000, -1064, -1)) * 2
    image.service()
    bad = bytearray(block)
    bad[2] = 89
    assert image.submit(stereo, bad)[0] == 0x80070057
    assert image.submit(stereo, block[:-1])[0] == 0x80070057
    image.release(stereo)


def test_static_buffer(build):
    image = AudioImage(build)
    desc = image.alloc(struct.pack("<IIIIII", 24, 0, 8, image.format(channels=2), 3, 0))
    out = image.alloc(bytes(4))
    assert image.call("_DirectSoundCreateBuffer@8", desc, out) == 0
    buffer, = image.words(out)
    data = image.alloc(pcm(100, -100, 200, -200, 300, -300))
    assert image.call("_IDirectSoundBuffer_SetBufferData@12", buffer, data, 12) == 0
    assert image.call("_IDirectSoundBuffer_Play@16", buffer, 0, 0, 0) == 0
    assert image.render(4) == output_samples((100, -100, 200, -200, 300, -300, 0, 0))
    status = image.alloc(bytes(4))
    image.call("_IDirectSoundBuffer_GetStatus@8", buffer, status)
    assert image.words(status)[0] == 0
    image.call("_IDirectSoundBuffer_SetCurrentPosition@8", buffer, 0)
    image.call("_IDirectSoundBuffer_SetLoopRegion@12", buffer, 4, 8)
    image.call("_IDirectSoundBuffer_Play@16", buffer, 0, 0, 1)
    assert image.render(6) == output_samples((100, -100, 200, -200, 300, -300, 200, -200, 300, -300, 200, -200))
    image.call("_IDirectSoundBuffer_Stop@4", buffer)
    assert image.render(2) == (0,) * 4
    image.call("_IDirectSoundBuffer_SetCurrentPosition@8", buffer, 0)
    assert image.call("_IDirectSoundBuffer_SetPitch@8", buffer, 4096) == 0
    image.call("_IDirectSoundBuffer_Play@16", buffer, 0, 0, 0)
    assert image.render(2) == output_samples((100, -100, 300, -300))
    assert image.call("_IDirectSoundBuffer_SetLoopRegion@12", buffer, 8, 8) == 0x80070057
    assert image.call("_IDirectSoundBuffer_Release@4", buffer) == 0
    image.service()
    assert buffer in image.freed


def test_buffer_pitch_is_absolute(build):
    # progress_bar.c pitches 22.05 kHz buffers; pitch 0 means 48 kHz, not native.
    image = AudioImage(build)
    desc = image.alloc(struct.pack("<IIIIII", 24, 0, 6, image.format(rate=22050), 3, 0))
    out = image.alloc(bytes(4))
    assert image.call("_DirectSoundCreateBuffer@8", desc, out) == 0
    buffer, = image.words(out)
    data = image.alloc(pcm(100, 200, 300))
    assert image.call("_IDirectSoundBuffer_SetBufferData@12", buffer, data, 6) == 0
    assert image.call("_IDirectSoundBuffer_SetPitch@8", buffer, 0) == 0
    image.call("_IDirectSoundBuffer_Play@16", buffer, 0, 0, 0)
    assert image.render(3)[::2] == output_samples((100, 200, 300))
    assert image.call("_IDirectSoundBuffer_SetPitch@8", buffer, -8773) == 0
    assert image.call("_IDirectSoundBuffer_SetPitch@8", buffer, 8192) == 0x80070057
    assert image.call("_IDirectSoundBuffer_Release@4", buffer) == 0


def test_event_and_release(build):
    image = AudioImage(build)
    stream = image.stream(callback=False, capacity=1)
    assert image.submit(stream, pcm(700), 123)[0] == 0
    assert image.submit(stream, pcm(800), 124)[0] == 0x80070057
    assert image.render(1) == output_samples((700, 700))
    assert image.submit(stream, pcm(800), 124)[0] == 0x80070057
    image.service()
    assert image.events == [123]
    assert image.submit(stream, pcm(800), 124)[0] == 0
    image.release(stream)
    assert image.events == [123, 124]


def test_validation_and_reentrant_callbacks(build):
    image = AudioImage(build)
    out = image.alloc(bytes(4))
    invalid = image.alloc(struct.pack("<HHIIHHHH", 1, 3, 48000, 1, 6, 16, 0, 0))
    desc = image.alloc(struct.pack("<IIIIII", 0, 4, invalid, image.callback, 42, 0))
    assert image.call("_DirectSoundCreateStream@12", desc, out, 0) == 0x80070057
    assert image.words(out)[0] == 0
    stream = image.stream()
    assert image.call("_IDirectSoundStream_SetVolume@8", stream, 1) == 0x80070057
    assert image.call("_IDirectSoundStream_SetFrequency@8", stream, -1) == 0x80070057
    assert image.submit(stream, b"\x01")[0] == 0x80070057
    image.submit(stream, pcm(10), 1)
    image.submit(stream, pcm(20), 2)
    assert image.render(2) == output_samples((10, 10, 20, 20))
    # Releasing from a completion callback must allow recursive DoWork,
    # dispatch the remaining abort, and reclaim the object exactly once.
    release = image.words(image.words(stream)[0] + 4)[0]
    # Callback machine code: if packet context == 1, call Release(this),
    # then RET 12. Use actual 32-bit callback code for reentry, not a nested
    # Unicorn emu_start, which would replace the current emulation's stack.
    callback = image.alloc(b"\x83\x7c\x24\x08\x01\x75\x0c\x68" +
                           struct.pack("<I", stream) + b"\xb8" +
                           struct.pack("<I", release) + b"\xff\xd0\xc2\x0c\x00")
    # Replace callback pointer in the object. Derive its field offset from
    # the existing callback value to avoid coupling the test to voice size.
    object_bytes = bytes(image.uc.mem_read(stream, 0x400))
    offset = object_bytes.index(struct.pack("<I", image.callback))
    image.uc.mem_write(stream + offset, struct.pack("<I", callback))
    image.service()
    assert image.freed == [stream]
    assert image.render(1) == (0, 0)


def test_game_channel_initialization(build, image_type=AudioImage, playback=None):
    image = image_type(build)
    controls = []
    descriptions = []

    def missing():
        raise AssertionError("game initialization reached a missing compatibility function")

    def describe_stream():
        _, desc, _, _ = image.args(4)
        flags, capacity, format_address, callback, context, _ = image.words(desc, 6)
        format_fields = struct.unpack("<HHIIHHHH", image.uc.mem_read(format_address, 20))
        descriptions.append((flags, capacity, callback, context, format_fields))

    image.hooks[image.symbols["_halo_standalone_missing"]] = missing
    image.hooks[image.symbols["_display_assert"]] = missing
    image.hooks[image.symbols["_IDirectSound_CreateSoundStream@16"]] = describe_stream
    image.hooks[image.symbols["_FUN_001ca970@12"]] = image.completed
    for name, size in (("SetMixBins", 8), ("SetMixBinVolumes_12", 12),
                       ("SetMode", 12), ("SetPosition", 20), ("SetVelocity", 20),
                       ("SetConeOrientation", 20), ("SetMinDistance", 12),
                       ("SetMaxDistance", 12), ("SetConeAngles", 16),
                       ("SetConeOutsideVolume", 12), ("SetAllParameters", 12)):
        image.hooks[image.symbols[f"_IDirectSoundStream_{name}@{size}"]] = (
            lambda name=name: controls.append(name))

    for index, flags in enumerate((0, 2, 8, 9, 10, 12, 13, 14)):
        controls.clear()
        channel, stream = image.initialize_channel(flags, index)
        assert stream and struct.unpack("<h", image.uc.mem_read(channel + 2, 2))[0] == -1
        stream_flags, capacity, callback, context, fmt = descriptions[-1]
        assert (stream_flags, capacity, callback, context) == (
            0x10 if flags & 1 else 0, 4, image.symbols["_FUN_001ca970@12"], index)
        tag, channels, rate, _, align, bits, extra, samples = fmt
        if flags & 8:
            assert (tag, channels, rate, align, bits, extra, samples) == (
                0x69, 2 if flags & 2 else 1, 44100 if flags & 4 else 22050,
                72 if flags & 2 else 36, 4, 2, 64)
        else:
            assert (tag, channels, align, bits) == (1, 2, 4, 16)
        if flags & 1:
            assert set(controls) == {"SetMode", "SetPosition", "SetVelocity",
                                     "SetConeOrientation", "SetMinDistance", "SetMaxDistance",
                                     "SetConeAngles", "SetConeOutsideVolume", "SetAllParameters"}
        else:
            assert controls == ([] if flags & 2 else ["SetMixBins", "SetMixBinVolumes_12"])
        # Check the lift's initial mute, then the game's unity gain/pitch update.
        # Halo routes 2D channels without the stereo bit at -602 mB per bin.
        block = struct.pack("<hBB", 500, 0, 0) * channels + b"\x11" * (32 * channels)
        payload = block if flags & 8 else pcm(500, 500)
        bins = 0.5 if not flags & 3 else 1.0
        for gain in (0.0, 1.0):
            if gain:
                image.channel_gain(index, gain)
            result = image.submit(stream, payload, index)[0]
            assert result == 0, (flags, fmt, hex(result))
            if playback:
                playback(image, stream, payload, gain * bins * (1.0 if flags & 1 else 10 ** (-6 / 20)))
            else:
                expected = ((250, 250) if bins < 1 else (500, 500)) if gain else (0, 0)
                assert image.render(1) == (expected if flags & 1 else output_samples(expected))
            assert image.method(stream, 6) == 0
        image.release(stream)


def test_spatial_gain(build, image_type=AudioImage, gains=None):
    image = image_type(build)
    if gains is None:
        def gains(stream):
            image.submit(stream, pcm(10000))
            left, right = image.render(1)
            image.service()
            scale = 10000  # 3D voices have no default headroom.
            return left / scale, right / scale
    desc = image.alloc(struct.pack("<IIIIII", 0x10, 4, image.format(), image.callback, 42, 0))
    out = image.alloc(bytes(4))
    assert image.call("_DirectSoundCreateStream@12", desc, out, 0) == 0
    stream, = image.words(out)
    floats = lambda *values: [struct.unpack("<I", struct.pack("<f", v))[0] for v in values]
    ds = 0
    assert image.call("_IDirectSound_SetOrientation@32", ds, *floats(0, 0, 1, 0, 1, 0), 0) == 0
    assert image.call("_IDirectSoundStream_SetMinDistance@12", stream, *floats(2.0), 1) == 0
    # Deferred: no change until the per-frame commit.
    assert image.call("_IDirectSoundStream_SetPosition@20", stream, *floats(0, 0, 8), 1) == 0
    assert gains(stream) == (1.0, 1.0)
    assert image.call("_IDirectSound_CommitDeferredSettings@4", ds) == 0
    left, right = gains(stream)
    assert abs(left - 0.25) < 0.001 and left == right, (left, right)
    # Max distance clamps attenuation; rolloff scales it.
    assert image.call("_IDirectSoundStream_SetMaxDistance@12", stream, *floats(4.0), 0) == 0
    assert abs(gains(stream)[0] - 0.5) < 0.001
    assert image.call("_IDirectSound_SetRolloffFactor@12", ds, *floats(0.0), 0) == 0
    assert gains(stream) == (1.0, 1.0)
    assert image.call("_IDirectSound_SetRolloffFactor@12", ds, *floats(1.0), 0) == 0
    # A source on the listener's right shades the left ear.
    assert image.call("_IDirectSoundStream_SetPosition@20", stream, *floats(2, 0, 0), 0) == 0
    left, right = gains(stream)
    assert right == 1.0 and abs(left - 0.3) < 0.001, (left, right)
    # Disabled 3D and the listener moving on top of it both restore unity.
    assert image.call("_IDirectSound_SetPosition@20", ds, *floats(2, 0, 0), 0) == 0
    assert gains(stream) == (1.0, 1.0)
    assert image.call("_IDirectSoundStream_SetPosition@20", stream, *floats(100, 0, 0), 0) == 0
    assert image.call("_IDirectSoundStream_SetMode@12", stream, 2, 0) == 0
    assert gains(stream) == (1.0, 1.0)
    # I3DL2 direct level plus occlusion at its low-frequency ratio: -600 + -1000 * 0.2.
    params = image.alloc(struct.pack("<iiiififif", -600, 0, 0, 0, 0.0, -10000, 0.0, -1000, 0.2))
    assert image.call("_IDirectSoundStream_SetAllParameters@12", stream, params, 0) == 0
    left, right = gains(stream)
    assert abs(left - 0.3981) < 0.002 and left == right, (left, right)
    # Volume combines with the 3D level; mix bins select the stereo sides.
    assert image.call("_IDirectSoundStream_SetVolume@8", stream, -10000) == 0
    assert gains(stream) == (0.0, 0.0)
    assert image.call("_IDirectSoundStream_SetVolume@8", stream, 0) == 0
    volumes = image.alloc(struct.pack("<iii", -10000, 0, -10000))
    assert image.call("_IDirectSoundStream_SetMixBinVolumes_12@12", stream, 7, volumes) == 0
    left, right = gains(stream)
    assert left == 0.0 and abs(right - 0.3981) < 0.002
    assert image.call("_IDirectSoundStream_SetMixBins@8", stream, 1) == 0
    assert gains(stream) == (0.0, 0.0)
    image.release(stream)


def test_game_channel_packets(build):
    image = AudioImage(build)
    cache_refs = []
    image.hooks[image.symbols["_FUN_001bdd70"]] = lambda: image.return_from_hook(0x10000000)

    def add_cache_ref():
        cache_refs.append(image.args(1)[0])
        image.return_from_hook()

    image.hooks[image.symbols["_FUN_001be100"]] = add_cache_ref
    image.hooks[image.symbols["_FUN_001ca970@12"]] = image.completed
    # ADPCM mono, 22.05 kHz. Initialization must install a live per-channel
    # inner object and the game's callback; queueing splits two small blocks.
    channel, stream = image.initialize_channel(9, 7)
    # The lift starts muted; the game applies the sound's gain before playback.
    image.channel_gain(7, 1.0)
    block = struct.pack("<hBB", 500, 0, 0) + b"\x11" * 32
    data = image.alloc(block * 2)
    permutation = image.alloc(bytes(0x44))
    image.uc.mem_write(permutation + 0x30, struct.pack("<I", data))
    image.uc.mem_write(permutation + 0x40, struct.pack("<I", 72))
    image.uc.mem_write(channel + 0x68, struct.pack("<I", permutation))
    assert image.call("_dsound_channel_queue_packet", 7) & 255 == 1
    assert image.words(channel + 0x64)[0] == 36
    assert image.call("_dsound_channel_queue_packet", 7) & 255 == 1
    assert image.words(channel + 0x68)[0] == 0
    assert image.call("_dsound_channel_queue_packet", 7) & 255 == 0
    assert cache_refs == [permutation, permutation]
    output = image.render(300)
    assert output[:4] == (500, 500, 500, 500)  # Type 9 is 3D.
    assert output[-2:] == (0, 0)
    image.service()
    assert image.callbacks == [(7, permutation, 0), (7, permutation, 0)]
    image.release(stream)


def test_dma_retirement(build):
    image = AudioImage(build)
    # Current descriptor is still playing; the oldest one has retired.
    assert image.call("_audio_dma_retired", 1 | (2 << 8), 0, 2) == 1
    # On underrun CIV stays at LVI, so the last descriptor is also retired.
    assert image.call("_audio_dma_retired", 1 | (1 << 8) | (7 << 16), 0, 2) == 2
    # Wrap at descriptor 31 and reject impossible/outdated snapshots.
    assert image.call("_audio_dma_retired", 0 | (1 << 8), 31, 2) == 1
    assert image.call("_audio_dma_retired", 31 | (31 << 8) | (7 << 16), 30, 2) == 2
    assert image.call("_audio_dma_retired", 10 | (11 << 8), 0, 2) == 0
    assert image.call("_audio_dma_retired", 0 | (1 << 8), 0, 2) == 0


def test_game_channel_reuse(build, image_type=AudioImage, advance=None):
    image = image_type(build)
    # The optimizer can inline channel_get into the game's routines.
    channel = 0x4fdfc8
    vchannel = 0x4fdbc4
    image.uc.mem_write(channel, bytes(0x74))
    image.uc.mem_write(vchannel, bytes(4))
    image.uc.mem_write(0x4fdfc4, struct.pack("<h", 1))
    image.uc.mem_write(0x4fdbc2, struct.pack("<h", 1))
    image.uc.mem_write(0x505484, b"\x00")
    image.hooks[image.symbols["_sound_dsound_channel_get"]] = lambda: image.return_from_hook(channel)
    image.hooks[image.symbols["_sound_dsound_vchannel_get"]] = lambda: image.return_from_hook(vchannel)
    image.hooks[image.symbols["_FUN_001bdd70"]] = lambda: image.return_from_hook(0x10000000)
    image.hooks[image.symbols["_IDirectSound_CommitDeferredSettings@4"]] = lambda: image.return_from_hook(pops=4)
    refs = [0]

    def cache_ref(delta):
        refs[0] += delta
        assert refs[0] >= 0
        image.return_from_hook()

    image.hooks[image.symbols["_FUN_001be100"]] = lambda: cache_ref(1)
    image.hooks[image.symbols["_FUN_001be140"]] = lambda: cache_ref(-1)
    channel, stream = image.initialize_channel(8, 0)
    image.channel_gain(0, 1.0)
    block = struct.pack("<hBB", 500, 0, 0) + b"\x11" * 32
    data = image.alloc(block * 2)
    permutation = image.alloc(bytes(0x44))
    image.uc.mem_write(permutation + 0x30, struct.pack("<I", data))
    image.uc.mem_write(permutation + 0x40, struct.pack("<I", 72))

    def pending():
        return struct.unpack("<h", image.uc.mem_read(channel + 8, 2))[0]

    # Run the game's real queue, completion, stop and restart functions.
    # Reuse the same channel before the next frame's DirectSoundDoWork,
    # alternating partially played and already mixed packets.
    for shot in range(32):
        image.uc.mem_write(channel + 2, struct.pack("<h", 0))
        image.uc.mem_write(vchannel, struct.pack("<h", 0))
        image.call("_FUN_001cb0c0", 0, permutation)
        assert pending() == 2 and refs[0] == 2
        if advance is None:
            # Type 8 is 2D mono: Halo's mix bins play it at -602 mB per side.
            assert image.render(300 if shot % 2 else 1)[0] == output_samples((250,))[0]
        else:
            advance(image, stream, shot)
        image.call("_sound_dsound_channel_release", 0)
        assert image.call("_sound_dsound_channel_stop_check", 0) & 255 == 1
        assert pending() == 0 and refs[0] == 0, "channel reusable before its abort callbacks"
        image.service()
        assert pending() == 0 and refs[0] == 0
    image.release(stream)


def test_worker_without_spdif_callbacks(build):
    image = AudioImage(build)
    stream = image.stream()
    image.submit(stream, pcm(*([500] * 4096)))
    image.uc.mem_map(0xfec00000, 0x1000)
    image.uc.mem_write(image.symbols["_initialized"], struct.pack("<I", 1))
    buffers = [image.alloc(bytes(4096)) for _ in range(3)]
    image.uc.mem_write(image.symbols["_audio_buffers"], struct.pack("<III", *buffers))
    # No S/PDIF progress and no DPC signals. A single snapshot reports both
    # startup PCM descriptors drained. The worker must refill both buffers.
    snapshots = [1 | (1 << 8) | (7 << 16), 3 | (3 << 8), 4 | (4 << 8)]
    queued = []
    waits = []

    def wait():
        waits.append(image.args(2))
        assert waits[-1][1] == 1, "worker blocked on missing S/PDIF callback"
        if not snapshots:
            image.uc.emu_stop()
            return
        image.uc.mem_write(0xfec00114, struct.pack("<I", snapshots.pop(0)))
        image.return_from_hook(258)  # WAIT_TIMEOUT

    def provide():
        buffer, size, final = image.args(3)
        assert size == 4096 and final == 0
        queued.append((buffer, struct.unpack("<2048h", image.uc.mem_read(buffer, size))))
        image.return_from_hook()

    def mmio_read(uc, access, address, size, value, context):
        assert size == 4 and address in (0xfec00114, 0xfec00174), "unsupported MMIO read"

    image.hooks[image.symbols["_WaitForSingleObject"]] = wait
    image.hooks[image.symbols["_XAudioProvideSamples"]] = provide
    # O2 inlines the transport's local lock wrappers into the worker.
    image.hooks[image.symbols["_EnterCriticalSection"]] = image.lock
    image.hooks[image.symbols["_LeaveCriticalSection"]] = image.unlock
    image.uc.hook_add(UC_HOOK_MEM_READ, mmio_read, begin=0xfec00000, end=0xfec00fff)
    esp = 0x200ff000
    image.uc.mem_write(esp, struct.pack("<II", 0x30000000, 0))
    image.uc.reg_write(UC_X86_REG_ESP, esp)
    image.uc.emu_start(image.symbols["_audio_worker@4"], 0x30000000, count=2000000)
    assert len(queued) == 4 and len(waits) == 4
    assert [p[0] for p in queued] == buffers + buffers[:1]
    assert all(samples == output_samples((500,) * 2048) for _, samples in queued)
    stats = image.words(image.symbols["_halo_audio_transport_stats"], 8)
    assert stats[0] == 0 and stats[1] == 4 and stats[5] == 1 and stats[6] == 4
    assert not image.locked


if __name__ == "__main__":
    build = Path(sys.argv[1] if len(sys.argv) > 1 else "build-seed")
    for test in (test_output_clipping, test_stream_lifecycle, test_capacity_flush_pause, test_resampling_volume_mix,
                 test_adpcm, test_static_buffer, test_buffer_pitch_is_absolute, test_event_and_release,
                 test_validation_and_reentrant_callbacks, test_game_channel_initialization,
                 test_spatial_gain,
                 test_game_channel_packets,
                 test_game_channel_reuse, test_dma_retirement, test_worker_without_spdif_callbacks):
        test(build)
        print(f"PASS {test.__name__}")
    print("Linked 32-bit audio checks passed; AC97/device playback remains unverified.")
