#!/usr/bin/env python3
"""Run the linked APU adapter with simulated driver events, not APU emulation.

python3 tools/test_audio_apu.py build-seed
Requires pefile and unicorn. The DPC pinning check runs the real driver DPC.
"""
import math
import re
import struct
import sys
from pathlib import Path

from test_audio import (HEADROOM, AudioImage, floats, pcm, test_game_channel_reuse, test_game_channel_initialization)


class ApuImage(AudioImage):
    def __init__(self, build):
        super().__init__(build)
        assert self.symbols["__tls_array"] == 4, "CRT TLS offset replaced by a stub"
        self.voices = {}
        self.stop_timeout = False
        self.irql = 0
        self.sleeps = 0
        self.uc.mem_map(0xfe800000, 0x30000)
        self.write(0xfe820010, 0x1000)  # PIO FIFO space; DPC inlines its wait.
        for name, handler in {
            "nxAudioVoiceCreate": self.create_voice,
            "nxAudioVoiceDestroy": self.destroy_voice,
            "nxAudioVoiceStop": self.stop_voice,
            "nxAudioVoiceStart": self.start_voice,
            "nxAudioVoicePause": self.pause_voice,
            "nxAudioVoiceSetChannelGain": self.set_channels,
            "nxAudioVoiceSetPitch": lambda: self.set_float("pitch"),
            "nxAudioBufferQueue": self.queue,
            "nxAudioBufferSubmit": self.attach,
            "nxAudioVoiceSetLooping": self.loop,
            "nxAudioVoiceSetPlaybackOffset": self.seek,
            "apu_wait_for_pio": lambda: self.return_from_hook(),
            "Sleep": self.sleep,
            "DbgPrint": lambda: self.return_from_hook(),
        }.items():
            self.hooks[self.symbols["_" + name]] = handler
        # Kernel calls use an import pointer; test addresses replace those
        # pointers so ordinal thunks cannot execute privileged instructions.
        self.import_hook("__imp__KeRaiseIrqlToDpcLevel@0", self.raise_irql)
        self.import_hook("__imp_@KfLowerIrql@4", self.lower_irql)
        self.import_hook("__imp__MmAllocateContiguousMemory@4", self.contiguous)
        self.import_hook("__imp__MmFreeContiguousMemory@4", self.free_contiguous)
        # Pages map apart unless a test opts in, so packets spanning a page
        # take the contiguous copy path.
        self.contiguous_pages = False
        self.copies = 0
        self.import_hook("__imp__MmGetPhysicalAddress@4", self.physical)
        self.import_hook("__imp__DbgPrint", lambda: self.return_from_hook())

    def physical(self):
        address, = self.args(1)
        self.return_from_hook(address if self.contiguous_pages else address * 2 & 0xffffffff, pops=4)

    def import_hook(self, name, handler):
        address = self.alloc(b"\xc3")
        self.uc.mem_write(self.symbols[name], struct.pack("<I", address))
        self.hooks[address] = handler

    def write(self, address, *words):
        self.uc.mem_write(address, struct.pack("<" + "I" * len(words), *words))

    def hardware(self, stream):
        # These are compile-time x86 ABI offsets, checked by creation/queue
        # assertions below; no host-sized C structure is used in this test.
        return self.words(stream + 0x38 + 480)[0]

    def create_voice(self):
        voice, format_address = self.args(2)
        self.voices[voice] = {"destroyed": False, "queues": [], "payloads": [], "gain": (1.0, 1.0), "pitch": 1.0,
                              "type": self.words(format_address + 12)[0]}
        self.write(voice, 3)  # NX_STOPPED
        self.uc.mem_write(voice + 4, bytes(self.uc.mem_read(format_address, 16)))
        self.write(voice + 20, 0, 0)
        self.uc.mem_write(voice + 137, b"\x00")  # next_ssl_index
        self.return_from_hook(1)

    def destroy_voice(self):
        voice, = self.args(1)
        assert self.words(voice)[0] == 3, "destroy before stop handshake"
        self.voices[voice]["destroyed"] = True
        self.write(voice + 20, 0, 0)
        self.return_from_hook()

    def stop_voice(self):
        voice, = self.args(1)
        self.write(voice, 2 if self.stop_timeout else 3)
        self.return_from_hook(1)

    def start_voice(self):
        voice, = self.args(1)
        assert self.irql == 0, "FPU/voice start at DPC level"
        self.write(voice, 0)
        self.return_from_hook(1)

    def pause_voice(self):
        voice, = self.args(1)
        if self.words(voice)[0] != 3:
            self.write(voice, 1)
        self.return_from_hook(1)

    def set_float(self, key):
        voice, raw = self.args(2)
        assert self.irql == 0, "gain/pitch work at DPC level"
        self.voices[voice][key] = struct.unpack("<f", struct.pack("<I", raw))[0]
        self.return_from_hook(1)

    def set_channels(self):
        voice, *raw = self.args(7)
        assert self.irql == 0, "gain work at DPC level"
        gains = [struct.unpack("<f", struct.pack("<I", value))[0] for value in raw]
        assert gains[2:] == [0.0] * 4, "adapter routed audio beyond the front bins"
        self.voices[voice]["gain"] = tuple(gains[:2])
        self.return_from_hook(1)

    def queue(self):
        voice, buffer = self.args(2)
        assert self.irql == 2
        slots = self.words(voice + 20, 2)
        slot = self.uc.mem_read(voice + 137, 1)[0]
        assert slots[slot] == 0, "driver FIFO slot is still occupied"
        payload, size = self.words(buffer, 2)
        assert payload and size
        self.voices[voice]["queues"].append(bytes(self.uc.mem_read(payload, size)))
        self.voices[voice]["payloads"].append(payload)
        self.write(voice + 20 + 4 * slot, buffer)
        self.uc.mem_write(voice + 137, bytes([slot ^ 1]))
        self.return_from_hook(1)

    def attach(self):
        voice, buffer = self.args(2)
        assert self.words(voice)[0] == 3, "static submission to active voice"
        self.write(voice + 20, buffer)
        self.return_from_hook(1)

    def loop(self):
        voice, looping = self.args(2)
        self.voices[voice]["loop"] = looping
        self.return_from_hook(1)

    def seek(self):
        voice, offset = self.args(2)
        self.voices[voice]["offset"] = offset
        self.return_from_hook(1)

    def complete(self, stream, slots=(0, 1)):
        voice = self.hardware(stream)
        assert voice in self.voices
        for slot in slots:
            self.write(voice + 20 + 4 * slot, 0)

    def raise_irql(self):
        previous = self.irql
        self.irql = 2
        self.return_from_hook(previous)

    def lower_irql(self):
        from unicorn.x86_const import UC_X86_REG_ECX
        self.irql = self.uc.reg_read(UC_X86_REG_ECX) & 255
        self.return_from_hook()

    def contiguous(self):
        size, = self.args(1)
        self.copies += 1
        self.return_from_hook(self.alloc(bytes(size)), pops=4)

    def free_contiguous(self):
        address, = self.args(1)
        assert address not in self.freed
        self.freed.append(address)
        self.return_from_hook(pops=4)

    def sleep(self):
        assert self.irql == 0
        self.sleeps += 1
        self.return_from_hook()


def test_four_packets_and_coalesced_completion(build):
    image = ApuImage(build)
    stream = image.stream()
    packets = [image.submit(stream, pcm(i), i) for i in range(4)]
    hardware = image.hardware(stream)
    assert all(packet[0] == 0 for packet in packets)
    assert image.voices[hardware]["queues"] == [pcm(0), pcm(1)]
    assert image.submit(stream, pcm(4))[0] == 0x80070057
    image.complete(stream)
    image.call("_standalone_audio_poll")
    assert image.callbacks == [], "callback from backend worker"
    assert image.voices[hardware]["queues"] == [pcm(i) for i in range(4)]
    assert all(image.words(packet[2])[0] == 0x8000000a for packet in packets)
    image.service()
    assert image.callbacks == [(42, 0, 0), (42, 1, 0)]
    image.complete(stream)
    image.service()
    assert image.callbacks == [(42, i, 0) for i in range(4)]
    assert all(image.words(packet[1])[0] == 2 and image.words(packet[2])[0] == 0 for packet in packets)
    image.release(stream)


def test_contiguous_packets_play_in_place(build):
    # Halo's sound cache is physically contiguous; only packets that span
    # pages mapped apart need a contiguous copy.
    image = ApuImage(build)
    stream = image.stream()
    data = pcm(*range(3000))
    image.submit(stream, data, 0)
    assert image.copies == 1
    image.contiguous_pages = True
    payload = image.next_memory
    image.submit(stream, data, 1)
    assert image.copies == 1
    hardware = image.hardware(stream)
    assert image.voices[hardware]["queues"] == [data, data]
    assert image.voices[hardware]["payloads"][1] == payload
    image.complete(stream)
    image.service()
    assert image.callbacks == [(42, 0, 0), (42, 1, 0)]
    image.release(stream)


def test_completion_before_next_process(build):
    image = ApuImage(build)
    stream = image.stream()
    image.submit(stream, pcm(1), 1)
    image.submit(stream, pcm(2), 2)
    image.complete(stream, (0,))
    # Process runs before the worker: it must retire the old owner before
    # assigning its now-free hardware slot to packet 3.
    image.submit(stream, pcm(3), 3)
    image.service()
    assert image.callbacks == [(42, 1, 0)]
    image.complete(stream)
    image.service()
    assert image.callbacks == [(42, i, 0) for i in (1, 2, 3)]
    image.release(stream)


def test_reference_voice_headroom(build):
    # Inspect the real PIO writes for slots 0 and 1. cachebeta 0x2054a8: 2D
    # defaults to 600 mB through the linked driver's gain conversion. A
    # DS_CTRL3D voice has none; its slot 1 is a rear bin, muted for stereo.
    for flags, headroom, rear in ((0, 384, 384), (0x10, 0, 0xfff)):
        image = ApuImage(build)
        desc = image.alloc(struct.pack("<6I", flags, 4, image.format(), image.callback, 42, 0))
        out = image.alloc(bytes(4))
        assert image.call("_DirectSoundCreateStream@12", desc, out, 0) == 0
        stream, = image.words(out)
        image.submit(stream, pcm(10000))
        hardware = image.hardware(stream)
        # Complete the voice defaults omitted by the lifecycle mock.
        image.write(hardware + 36, 0x3f800000)  # master_gain = 1
        image.write(hardware + 168, 0xffff)  # HRTF_NULL_HANDLE
        image.hooks.pop(image.symbols["_nxAudioVoiceSetChannelGain"])
        assert image.call("_audio_output_volume", stream + 0x38) == 1

        def levels():
            register = image.words(0xfe820360)[0]
            return (register >> 4) & 0xfff, (register >> 20) & 0xfff

        front, back = levels()
        assert abs(front - headroom) <= 1 and abs(back - rear) <= 1, levels()
        assert image.call("_IDirectSoundStream_SetVolume@8", stream, -600) == 0
        front, back = levels()
        assert abs(front - headroom - 384) <= 1 and abs(back - min(rear + 384, 0xfff)) <= 1, levels()
        assert image.call("_IDirectSoundStream_SetVolume@8", stream, -10000) == 0
        assert levels() == (0xfff, 0xfff)
        image.complete(stream)
        image.service()
        image.release(stream)


def test_pause_gain_pitch_and_adpcm(build):
    image = ApuImage(build)
    stream = image.stream(image.format(rate=22050, adpcm=True))
    assert image.call("_IDirectSoundStream_Pause@8", stream, 1) == 0
    block = struct.pack("<hBB", 500, 0, 0) + b"\x11" * 32
    assert image.submit(stream, block)[0] == 0
    hardware = image.hardware(stream)
    assert image.words(hardware)[0] == 3
    assert image.voices[hardware]["queues"] == [block]
    assert image.call("_IDirectSoundStream_SetFrequency@8", stream, 44100) == 0
    assert image.call("_IDirectSoundStream_SetVolume@8", stream, -600) == 0
    assert image.voices[hardware]["pitch"] == 2.0
    left, right = image.voices[hardware]["gain"]
    assert 0.2511 <= left <= 0.2512 and left == right
    assert image.call("_IDirectSoundStream_Pause@8", stream, 0) == 0
    assert image.words(hardware)[0] == 0
    image.complete(stream)
    image.service()
    image.release(stream)


def test_stop_timeout_retains_ownership(build):
    image = ApuImage(build)
    stream = image.stream()
    _, done, status = image.submit(stream, pcm(800))
    image.stop_timeout = True
    assert image.method(stream, 1) == 1
    assert image.sleeps == 150
    assert not image.voices[image.hardware(stream)]["destroyed"]
    assert not image.freed and not image.callbacks
    assert image.words(done)[0] == 0 and image.words(status)[0] == 0x8000000a
    stream_status = image.alloc(bytes(4))
    assert image.method(stream, 3, stream_status) == 0
    assert not image.words(stream_status)[0] & 1, "stopping stream advertised READY"
    assert image.submit(stream, pcm(900))[0] == 0x80070057
    image.stop_timeout = False
    image.release(stream)
    assert image.callbacks == [(42, 99, 0x80004004)]


def test_stop_timeout_late_completion(build):
    image = ApuImage(build)
    stream = image.stream()
    old = [image.submit(stream, pcm(i), i) for i in range(4)]
    spare = image.hardware(stream)
    image.stop_timeout = True
    # Flush hands the stream to a second voice at once, as retail does. The
    # stopping voice keeps its copies until the driver reports it idle.
    assert image.method(stream, 6) == 0
    assert image.sleeps == 0 and not image.freed
    assert image.callbacks == [(42, i, 0x80004004) for i in range(4)]
    assert all(image.words(status)[0] == 0x80004004 for _, _, status in old)
    # Halo flushes twice per new sound; the voiceless second Flush must not wait.
    assert image.method(stream, 6) == 0 and image.sleeps == 0
    packets = [image.submit(stream, pcm(i), i) for i in range(4, 8)]
    hardware = image.hardware(stream)
    assert hardware != spare and not image.voices[spare]["destroyed"]
    image.call("_standalone_audio_poll")
    assert not image.voices[spare]["destroyed"]
    # A second Flush before the first voice idles waits like Release.
    image.callbacks.clear()
    assert image.method(stream, 6) == 0x80004005
    assert not image.voices[hardware]["destroyed"]
    assert not image.callbacks and not image.freed
    image.call("_standalone_audio_poll")
    assert not image.voices[hardware]["destroyed"]
    # The idle DPC can arrive after Flush timed out, leaving packets which
    # never reached either hardware slot. Halo waits on the active bit before
    # retrying Flush, so polling must retire those packets without callbacks.
    image.write(spare, 3)
    image.write(hardware, 3)
    image.call("_standalone_audio_poll")
    assert image.words(stream + 0x30)[0] == 0, "late stop left stream active"
    assert image.voices[hardware]["destroyed"] and image.voices[spare]["destroyed"]
    assert not image.callbacks
    image.service()
    assert image.callbacks == [(42, i, 0x80004004) for i in range(4, 8)]
    assert all(image.words(status)[0] == 0x80004004 for _, _, status in packets)
    assert image.submit(stream, pcm(7), 7)[0] == 0
    image.complete(stream)
    image.service()
    image.stop_timeout = False
    image.release(stream)


def test_halo_channel_reuse(build):
    def advance(image, stream, shot):
        if shot % 2:
            image.complete(stream)
            image.call("_standalone_audio_poll")
    test_game_channel_reuse(build, ApuImage, advance)


def test_halo_channel_initialization(build):
    def playback(image, stream, payload, gain):
        voice = image.voices[image.hardware(stream)]
        if voice["type"] == 3:
            # A 3D channel's front slot, without the 2D headroom.
            front = image.words(0xfe820360)[0] >> 4 & 0xfff
            assert front == (0 if gain else 0xfff), (hex(front), gain)
        else:
            assert all(abs(g - gain) < 0.001 for g in voice["gain"]), (voice["gain"], gain)
        assert voice["queues"] == [payload]
    test_game_channel_initialization(build, ApuImage, playback)


def hrtf_tables(build):
    text = (Path(build) / "generated" / "xdk_hrtf.h").read_text()
    rows = [[int(v) for v in row.split(",")] for row in re.findall(r"^\{([-\d,]+)\},$", text, re.M)]
    return rows[:-61], rows[-61:]


def hrir_words(left, right, itd):
    words = [left[2 * i] | right[2 * i] << 8 | left[2 * i + 1] << 16 | right[2 * i + 1] << 24 for i in range(15)]
    return words + [(left[30] | right[30] << 8 | itd << 25) & 0xffffffff]


def test_halo_spatial_registers(build):
    # cachebeta's first ambient 3D voice (halo docs/xdk/dsound-3d.md, section
    # 10): lVolume -602 and Room = RoomHF = -1204 at pseudo-angles of 18 deg
    # right, 6 deg down. Retail wrote these registers for it.
    image = ApuImage(build)
    pio = lambda method: image.words(0xfe820000 + method)[0]
    filters, index = hrtf_tables(build)
    desc = image.alloc(struct.pack("<6I", 0x10, 4, image.format(), image.callback, 42, 0))
    out = image.alloc(bytes(4))
    assert image.call("_DirectSoundCreateStream@12", desc, out, 0) == 0
    stream, = image.words(out)
    ds = 0
    assert image.call("_IDirectSound_SetOrientation@32", ds, *floats(0, 0, 1, 0, 1, 0), 1) == 0
    # Within MinDistance (1.0), as v0 was.
    height = -1 / 15 * math.sqrt(1.16)
    for side in (1, -1):
        assert image.call("_IDirectSoundStream_SetPosition@20", stream, *floats(0.2 * side, height, 0.5), 1) == 0
        params = image.alloc(struct.pack("<iiiififif", 0, 0, -1204, -1204, 0.0, 0, 0.0, 0, 0.25))
        assert image.call("_IDirectSoundStream_SetAllParameters@12", stream, params, 1) == 0
        assert image.call("_IDirectSoundStream_SetVolume@8", stream, -602) == 0
        # Starting the voice commits the deferred settings.
        image.submit(stream, pcm(10000))
        hardware = image.hardware(stream)
        assert image.voices[hardware]["type"] == 3  # NX_VOICE_TYPE_3D_STREAM
        assert (pio(0x360), pio(0x364), pio(0x368)) == (0xffff181f, 0xffff181f, 0xffff483f)
        assert (pio(0x374), pio(0x378)) == (0, 0xd8e10000)
        assert pio(0x318) >> 16 & 3 == 3
        # The stream's first voice wrote entry 0 for the origin at creation,
        # so this pair takes entry 1; the flushed stream's next voice starts at 0.
        assert pio(0x160) == pio(0x31c) == (1 if side > 0 else 0)
        first = index[18 // 3][(-6 + 90) // 6]
        near, far = filters[first], filters[first + 1]
        s8 = lambda row: [v - 256 if v > 127 else v for v in row[:31]]
        expected = hrir_words(near, far, near[31]) if side > 0 else hrir_words(far, near, -near[31])
        assert [pio(0x400 + 4 * i) for i in range(16)] == expected
        assert s8(near) != s8(far)
        image.complete(stream)
        image.service()
        # A flush retires the hardware voice, so the next start makes a new one.
        assert image.call("_IDirectSoundBuffer_Stop@4", stream) == 0
    # Distance, the reverb send and Doppler at 3.048 m per unit: a source
    # 5 units ahead loses 20 log10(5) dB and, moving away at 10 units/s,
    # plays at 1 - 3.048 / 342 * 10 of its rate.
    assert image.call("_IDirectSound_SetDistanceFactor@12", ds, *floats(3.048), 0) == 0
    assert image.call("_IDirectSoundStream_SetPosition@20", stream, *floats(0, 0, 5), 0) == 0
    assert image.call("_IDirectSoundStream_SetVelocity@20", stream, *floats(0, 0, 10), 0) == 0
    image.submit(stream, pcm(10000))
    hardware = image.hardware(stream)
    assert (pio(0x360), pio(0x368)) == (0xffff4fff, 0xffff801f)
    assert abs(image.voices[hardware]["pitch"] - (1 - 3.048 / 342 * 10)) < 1e-4
    assert image.call("_IDirectSound_SetVelocity@20", ds, *floats(0, 0, 10), 0) == 0
    assert abs(image.voices[hardware]["pitch"] - 1) < 1e-6
    image.complete(stream)
    image.service()
    image.release(stream)


def test_static_buffer(build):
    image = ApuImage(build)
    desc = image.alloc(struct.pack("<IIIIII", 24, 0, 8, image.format(channels=2), 3, 0))
    out = image.alloc(bytes(4))
    assert image.call("_DirectSoundCreateBuffer@8", desc, out) == 0
    buffer, = image.words(out)
    data = image.alloc(pcm(100, -100, 200, -200))
    assert image.call("_IDirectSoundBuffer_SetBufferData@12", buffer, data, 8) == 0
    assert image.call("_IDirectSoundBuffer_SetLoopRegion@12", buffer, 4, 4) == 0
    assert image.call("_IDirectSoundBuffer_Play@16", buffer, 0, 0, 1) == 0
    hardware = image.hardware(buffer)
    assert image.voices[hardware]["loop"] == 1
    assert image.call("_IDirectSoundBuffer_SetCurrentPosition@8", buffer, 4) == 0
    assert image.voices[hardware]["offset"] == 4
    assert image.call("_IDirectSoundBuffer_Play@16", buffer, 0, 0, 0) == 0
    assert image.voices[hardware]["loop"] == 0
    assert image.call("_IDirectSoundBuffer_Stop@4", buffer) == 0
    assert image.voices[hardware]["destroyed"]
    assert image.call("_IDirectSoundBuffer_Play@16", buffer, 0, 0, 0) == 0
    image.release(buffer)


def test_buffer_pitch_is_absolute(build):
    # XDK pitch 4096 * log2(f / 48000) is independent of the buffer's rate.
    image = ApuImage(build)
    desc = image.alloc(struct.pack("<IIIIII", 24, 0, 8, image.format(rate=22050), 3, 0))
    out = image.alloc(bytes(4))
    assert image.call("_DirectSoundCreateBuffer@8", desc, out) == 0
    buffer, = image.words(out)
    data = image.alloc(pcm(100, -100, 200, -200))
    assert image.call("_IDirectSoundBuffer_SetBufferData@12", buffer, data, 8) == 0
    assert image.call("_IDirectSoundBuffer_Play@16", buffer, 0, 0, 1) == 0
    hardware = image.hardware(buffer)
    assert image.call("_IDirectSoundBuffer_SetPitch@8", buffer, -4096) == 0
    assert abs(image.voices[hardware]["pitch"] - 24000 / 22050) < 1e-4
    assert image.call("_IDirectSoundBuffer_SetPitch@8", buffer, -8773) == 0
    image.release(buffer)


def test_adpcm_loop_region_in_samples(build):
    # LBO/EBO count samples for ADPCM too (64 per 36-byte mono block).
    image = ApuImage(build)
    desc = image.alloc(struct.pack("<IIIIII", 24, 0, 144,
                                   image.format(rate=22050, adpcm=True), 3, 0))
    out = image.alloc(bytes(4))
    assert image.call("_DirectSoundCreateBuffer@8", desc, out) == 0
    buffer, = image.words(out)
    data = image.alloc(bytes(144))
    assert image.call("_IDirectSoundBuffer_SetBufferData@12", buffer, data, 144) == 0
    assert image.call("_IDirectSoundBuffer_Play@16", buffer, 0, 0, 1) == 0
    lbo, ebo = 0xfe820000 + 0x3a4, 0xfe820000 + 0x3dc
    assert image.words(lbo)[0] == 0 and image.words(ebo)[0] == 255
    assert image.call("_IDirectSoundBuffer_SetLoopRegion@12", buffer, 36, 72) == 0
    assert image.words(lbo)[0] == 64 and image.words(ebo)[0] == 191
    image.release(buffer)


def test_driver_descriptor_retirement_and_pinning(build):
    from unicorn import UC_HOOK_MEM_WRITE
    image = ApuImage(build)
    # Real compiled DPC, with only the page-lock/idle-list kernel boundary
    # simulated. A sticky success notifier must never unlock static pages.
    voice = image.alloc(bytes(256))
    data = image.alloc(pcm(100))
    buffer = image.alloc(struct.pack("<II", data, 2))
    image.write(voice + 20, buffer, 0)
    notifiers = image.alloc(bytes(16 * 1026))
    image.uc.mem_write(notifiers + 2 * 16 + 15, b"\x01")
    image.write(image.symbols["_g_hw_notifiers"], notifiers)
    image.write(image.symbols["_g_tracked_voices"], voice)
    image.write(image.symbols["_g_voice_allocation_mask"], 0xfffffffe, *([0xffffffff] * 7))
    unlocks = []
    descriptors = [1, 1]

    def pio_write(uc, access, address, size, value, context):
        if address in (0xfe820320, 0xfe82035c):
            assert size == 4
            descriptors[0 if address == 0xfe820320 else 1] = value

    image.uc.hook_add(UC_HOOK_MEM_WRITE, pio_write, begin=0xfe800000, end=0xfe82ffff)

    def unlock_pages():
        # The page release must follow hardware descriptor retirement.
        assert descriptors[0] == 0, "completed SSL remains playable after unlock"
        unlocks.append(image.args(3))
        image.return_from_hook(pops=12)

    image.import_hook("__imp__MmLockUnlockBufferPages@12", unlock_pages)
    image.import_hook("__imp_@InterlockedPopEntrySList@4", lambda: image.return_from_hook())
    for _ in range(3):
        image.call("_apu_dpc@16", 0, 0, 0, 0)
    assert unlocks == [] and image.words(voice + 20)[0] == buffer
    assert descriptors == [1, 1], "static completion retired streaming descriptors"
    # nxAudioVoice.streaming is offset 77 in the pinned x86 layout.
    image.uc.mem_write(voice + 77, b"\x01")
    image.call("_apu_dpc@16", 0, 0, 0, 0)
    image.call("_apu_dpc@16", 0, 0, 0, 0)
    assert unlocks == [(data, 2, 1)] and image.words(voice + 20)[0] == 0
    assert descriptors == [0, 1]
    # Refill A, then finish B: only B must be invalidated this time.
    descriptors[0] = 1
    image.uc.mem_write(notifiers + 2 * 16 + 15, b"\x00")
    image.uc.mem_write(notifiers + 3 * 16 + 15, b"\x01")
    image.write(voice + 24, buffer)
    image.import_hook("__imp__MmLockUnlockBufferPages@12",
                      lambda: image.return_from_hook(pops=12))
    image.call("_apu_dpc@16", 0, 0, 0, 0)
    assert descriptors == [1, 0] and image.words(voice + 24)[0] == 0


def test_driver_pio_completion_interleaving(build):
    from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_WRITE
    from unicorn.x86_const import UC_X86_REG_EIP, UC_X86_REG_ESP

    # Run real driver functions, not the adapter's gain/pitch mocks. Inject
    # the real completion DPC after selecting voice 64. It selects voice 65
    # to retire a buffer; the interrupted transaction must still target 64.
    def run(method, arguments, succeeds=True):
        image = ApuImage(build)
        image.hooks.pop(image.symbols["_" + method], None)
        target, completed = [image.alloc(bytes(256)) for _ in range(2)]
        for voice, index in ((target, 64), (completed, 65)):
            # Pinned driver's x86 nxAudioVoice ABI, as in the DPC test above.
            image.write(voice, 3)  # NX_STOPPED
            image.write(voice + 4, 48000)
            image.uc.mem_write(voice + 8, b"\x01\x02")  # mono PCM16
            image.write(voice + 36, 0x3f800000)  # master_gain = 1
            image.write(voice + 72, 0x3f800000)  # pitch = 1
            image.uc.mem_write(voice + 136, bytes([index]))
            image.write(voice + 168, 0xffff)  # HRTF_NULL_HANDLE
        image.write(image.symbols["_g_hw_voices"], image.alloc(bytes(128 * 256)))
        payload = image.alloc(pcm(100))
        buffer = image.alloc(struct.pack("<II", payload, 2))
        image.write(completed + 20, buffer, 0)
        image.uc.mem_write(completed + 77, b"\x01")  # streaming
        notifiers = image.alloc(bytes(16 * 1026))
        image.uc.mem_write(notifiers + (2 + 65 * 4) * 16 + 15, b"\x01")
        image.write(image.symbols["_g_hw_notifiers"], notifiers)
        image.write(image.symbols["_g_tracked_voices"] + 65 * 4, completed)
        image.write(image.symbols["_g_voice_allocation_mask"],
                    0xffffffff, 0xffffffff, 0xfffffffd, *([0xffffffff] * 5))
        image.import_hook("__imp__MmLockUnlockBufferPages@12",
                          lambda: image.return_from_hook(pops=12))
        image.import_hook("__imp__MmGetPhysicalAddress@4",
                          lambda: image.return_from_hook(image.args(1)[0], pops=4))
        image.import_hook("__imp_@InterlockedPopEntrySList@4",
                          lambda: image.return_from_hook())
        errors = []

        def set_error():
            # Last-error storage uses kernel TLS, which this harness omits.
            errors.append(image.args(1)[0])
            image.return_from_hook()

        image.hooks[image.symbols["_apu_set_last_error"]] = set_error
        state = {"selected": None, "pending": False, "delivered": False,
                 "context": None, "writes": 0}

        def pio_write(uc, access, address, size, value, context):
            if address == 0xfe8202f8:  # SET_CURRENT_VOICE
                state["selected"] = value
                if value == 64 and not state["delivered"]:
                    state["pending"] = True
            elif state["context"] is None:
                assert state["selected"] == 64, f"{method}: write redirected to another voice"
                state["writes"] += 1

        def schedule_dpc(uc, address, size, context):
            if address == 0x30000020 and state["context"] is not None:
                uc.context_restore(state["context"])
                uc.reg_write(UC_X86_REG_EIP, state["resume"])
                image.irql = state["previous_irql"]
                state["context"] = None
            elif state["pending"] and image.irql < 2:
                state["pending"] = False
                state["delivered"] = True
                state["context"] = uc.context_save()
                state["resume"] = uc.reg_read(UC_X86_REG_EIP)
                state["previous_irql"] = image.irql
                image.irql = 2
                uc.mem_write(0x200fe000, struct.pack("<5I", 0x30000020, 0, 0, 0, 0))
                uc.reg_write(UC_X86_REG_ESP, 0x200fe000)
                uc.reg_write(UC_X86_REG_EIP, image.symbols["_apu_dpc@16"])
            elif image.irql >= 2:
                # Gain/pitch calculations must precede the protected writes.
                opcode = uc.mem_read(address, 1)[0]
                assert not 0xd8 <= opcode <= 0xdf, f"{method}: x87 work at DPC level"

        image.uc.hook_add(UC_HOOK_MEM_WRITE, pio_write, begin=0xfe8202f8, end=0xfe82037f)
        image.uc.hook_add(UC_HOOK_CODE, schedule_dpc)
        try:
            result = image.call("_" + method, target, *arguments(image, buffer))
        except Exception as error:
            raise AssertionError(f"driver interleaving case: {method}") from error
        if method != "apu_flush_voice":
            assert bool(result & 255) == succeeds
        assert state["writes"] > 0 and state["delivered"], f"{method}: DPC was not exercised"
        assert errors == ([] if succeeds else [1])  # NX_AUDIO_ERR_OUT_OF_MEMORY
        assert image.irql == 0 and state["context"] is None
        assert image.words(completed + 20)[0] == 0, "deferred completion did not retire buffer"

    def floats(*values):
        return struct.unpack("<" + "I" * len(values), struct.pack("<" + "f" * len(values), *values))

    cases = {
        "nxAudioVoiceSetChannelGain": lambda image, buffer: floats(.2, .3, 0, 0, 0, 0),
        "nxAudioVoiceSetMasterGain": lambda image, buffer: floats(.4),
        "nxAudioVoiceSetHRTFGain": lambda image, buffer: floats(.4, .5),
        "nxAudioVoiceSetPitch": lambda image, buffer: floats(.8),
        "nxAudioVoiceSetLooping": lambda image, buffer: (1,),
        "nxAudioVoiceSetFormat": lambda image, buffer: (
            image.alloc(struct.pack("<IBB2xII", 48000, 1, 2, 0, 0)),),
        "nxAudioVoiceSetHRTFTarget": lambda image, buffer: (0,),
        "nxAudioVoiceSetPlaybackOffset": lambda image, buffer: (0,),
        "nxAudioVoiceSetAmplitudeEnvelope": lambda image, buffer: (image.alloc(bytes(24)),),
        "nxAudioVoiceSetFilterEnvelope": lambda image, buffer: (image.alloc(bytes(24)),),
        "nxAudioVoiceSetFilterCoefficients": lambda image, buffer: (0, 0, 0, 0, 0, 0),
        "apu_flush_voice": lambda image, buffer: (),
        "nxAudioBufferSubmit": lambda image, buffer: (buffer,),
    }
    for method, arguments in cases.items():
        run(method, arguments)

    def exhausted_sges(image, buffer):
        image.write(image.symbols["_g_sge_bitmap"], *([0xffffffff] * 64))
        return (buffer,)

    # Failed static submission must also lower IRQL and service completion.
    run("nxAudioBufferSubmit", exhausted_sges, succeeds=False)


def test_driver_stream_fifo_and_publication(build):
    from unicorn import UC_HOOK_MEM_WRITE

    for adpcm in (False, True):
        image = ApuImage(build)
        image.hooks.pop(image.symbols["_nxAudioBufferQueue"])
        voice = image.alloc(bytes(256))
        image.write(voice + 4, 22050)
        image.uc.mem_write(voice + 8, b"\x01\x00" if adpcm else b"\x01\x02")
        image.write(voice + 12, int(adpcm))  # codec
        image.uc.mem_write(voice + 77, b"\x01")  # streaming
        image.uc.mem_write(voice + 136, b"\x40")  # voice 64
        notifiers = image.alloc(bytes(16 * 1026))
        image.write(image.symbols["_g_hw_notifiers"], notifiers)
        image.write(image.symbols["_g_tracked_voices"] + 64 * 4, voice)
        image.write(image.symbols["_g_voice_allocation_mask"],
                    0xffffffff, 0xffffffff, 0xfffffffe, *([0xffffffff] * 5))
        image.import_hook("__imp__MmGetPhysicalAddress@4",
                          lambda: image.return_from_hook(image.args(1)[0], pops=4))
        image.import_hook("__imp_@InterlockedPopEntrySList@4",
                          lambda: image.return_from_hook())
        errors, page_events, publications = [], [], []
        state = {"locked": False}

        def page_lock():
            page_events.append(image.args(3))
            image.return_from_hook(pops=12)

        def set_error():
            errors.append(image.args(1)[0])
            image.return_from_hook()

        image.import_hook("__imp__MmLockUnlockBufferPages@12", page_lock)
        image.hooks[image.symbols["_apu_set_last_error"]] = set_error  # kernel TLS boundary

        def pio_write(uc, access, address, size, value, context):
            if address == 0xfe8202fc:
                state["locked"] = bool(value)
            elif address in (0xfe820320, 0xfe82035c) and value:
                assert state["locked"], "SSL published after unlocking voice"
                publications.append(0 if address == 0xfe820320 else 1)

        def notifier_write(uc, access, address, size, value, context):
            if value == 0:  # IN_PROGRESS
                assert state["locked"], "empty-slot notifier reset while APU can still process voice"

        image.uc.hook_add(UC_HOOK_MEM_WRITE, pio_write, begin=0xfe8202fc, end=0xfe82035f)
        image.uc.hook_add(UC_HOOK_MEM_WRITE, notifier_write,
                          begin=notifiers + (2 + 64 * 4) * 16 + 15,
                          end=notifiers + (3 + 64 * 4) * 16 + 15)

        def queue(marker, malformed=False):
            data = (struct.pack("<hBB", marker, 0, 0) + bytes(32)) if adpcm else pcm(marker)
            if malformed:
                data = data[:-1]
            payload = image.alloc(data)
            buffer = image.alloc(struct.pack("<II", payload, len(data)))
            result = image.call("_nxAudioBufferQueue", voice, buffer) & 255
            assert image.irql == 0 and not state["locked"]
            return result, buffer

        def complete(slot):
            image.uc.mem_write(notifiers + (2 + 64 * 4 + slot) * 16 + 15, b"\x01")
            image.call("_apu_dpc@16", 0, 0, 0, 0)
            assert image.words(voice + 20 + slot * 4)[0] == 0

        # Empty the stream after each packet. The APU waits at the other
        # slot, so every refill must alternate; first-free always chooses A.
        for marker in range(8):
            result, buffer = queue(marker)
            assert result == 1
            slot = marker & 1
            assert publications[-1] == slot, "stream FIFO reordered after starvation"
            assert image.words(voice + 20 + slot * 4)[0] == buffer
            complete(slot)

        # A busy next slot must reject queueing even if the other is free.
        assert queue(8)[0] and queue(9)[0]
        complete(1)
        before = len(page_events), len(publications)
        assert queue(10)[0] == 0
        assert errors.pop() == 6  # NX_AUDIO_ERR_QUEUE_FULL
        assert (len(page_events), len(publications)) == before
        complete(0)
        assert queue(10)[0] == 1 and publications[-1] == 0
        complete(0)

        # Failed descriptor construction must unpin and unlock without
        # advancing FIFO order; the next valid packet still goes to B.
        before = len(page_events), len(publications)
        assert queue(11, malformed=True)[0] == 0
        assert errors.pop() == 5  # NX_AUDIO_ERR_UNSUPPORTED
        assert len(publications) == before[1]
        assert len(page_events) == before[0] + 2
        assert page_events[-2][2] == 0 and page_events[-1][2] == 1
        assert queue(11)[0] == 1 and publications[-1] == 1
        complete(1)
        assert errors == []


def test_adapter_waits_for_stream_fifo_slot(build):
    image = ApuImage(build)
    stream = image.stream()
    image.submit(stream, pcm(1), 1)
    image.submit(stream, pcm(2), 2)
    # A delayed A completion leaves the next FIFO slot occupied, although
    # B is already free. Preserve the pending packet rather than abort it.
    image.complete(stream, (1,))
    image.submit(stream, pcm(3), 3)
    hardware = image.hardware(stream)
    assert image.voices[hardware]["queues"] == [pcm(1), pcm(2)]
    image.complete(stream, (0,))
    image.call("_standalone_audio_poll")
    assert image.voices[hardware]["queues"] == [pcm(1), pcm(2), pcm(3)]
    image.complete(stream, (0,))
    image.service()
    assert image.callbacks == [(42, i, 0) for i in (1, 2, 3)]
    image.release(stream)


if __name__ == "__main__":
    build = Path(sys.argv[1] if len(sys.argv) > 1 else "build-seed")
    for test in (test_four_packets_and_coalesced_completion, test_contiguous_packets_play_in_place,
                 test_completion_before_next_process,
                 test_reference_voice_headroom, test_pause_gain_pitch_and_adpcm, test_stop_timeout_retains_ownership,
                 test_stop_timeout_late_completion,
                 test_halo_channel_reuse, test_halo_channel_initialization, test_halo_spatial_registers,
                 test_static_buffer, test_buffer_pitch_is_absolute, test_adpcm_loop_region_in_samples,
                 test_driver_descriptor_retirement_and_pinning,
                 test_driver_pio_completion_interleaving,
                 test_driver_stream_fifo_and_publication, test_adapter_waits_for_stream_fifo_slot):
        test(build)
        print(f"PASS {test.__name__}")
    print("Linked APU adapter checks passed; audible output and timing require device validation.")
