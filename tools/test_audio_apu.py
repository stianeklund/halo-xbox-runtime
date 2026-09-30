#!/usr/bin/env python3
"""Run the linked APU adapter with simulated driver events, not APU emulation.

python3 tools/test_audio_apu.py build-apu
Requires pefile and unicorn. The DPC pinning check runs the real driver DPC.
"""
import struct
import sys
from pathlib import Path

from test_audio import AudioImage, pcm, test_game_channel_reuse


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
            "nxAudioVoiceSetMasterGain": lambda: self.set_float("gain"),
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
        self.import_hook("__imp__DbgPrint", lambda: self.return_from_hook())

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
        self.voices[voice] = {"destroyed": False, "queues": [], "gain": 1.0, "pitch": 1.0}
        self.write(voice, 3)  # NX_STOPPED
        self.uc.mem_write(voice + 4, bytes(self.uc.mem_read(format_address, 16)))
        self.write(voice + 20, 0, 0)
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

    def queue(self):
        voice, buffer = self.args(2)
        assert self.irql == 2
        slots = self.words(voice + 20, 2)
        assert 0 in slots, "driver queue overflow"
        payload, size = self.words(buffer, 2)
        assert payload and size
        self.voices[voice]["queues"].append(bytes(self.uc.mem_read(payload, size)))
        self.write(voice + 20 + 4 * slots.index(0), buffer)
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
    assert 0.501 <= image.voices[hardware]["gain"] <= 0.502
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
    packets = [image.submit(stream, pcm(i), i) for i in range(4)]
    hardware = image.hardware(stream)
    image.stop_timeout = True
    assert image.method(stream, 6) == 0x80004005
    assert not image.voices[hardware]["destroyed"]
    assert not image.callbacks and not image.freed
    image.call("_standalone_audio_poll")
    assert not image.voices[hardware]["destroyed"]
    # The idle DPC can arrive after Flush timed out, leaving packets which
    # never reached either hardware slot. Halo waits on the active bit before
    # retrying Flush, so polling must retire those packets without callbacks.
    image.write(hardware, 3)
    image.call("_standalone_audio_poll")
    assert image.words(stream + 0x30)[0] == 0, "late stop left stream active"
    assert image.voices[hardware]["destroyed"]
    assert not image.callbacks
    image.service()
    assert image.callbacks == [(42, i, 0x80004004) for i in range(4)]
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


if __name__ == "__main__":
    build = Path(sys.argv[1] if len(sys.argv) > 1 else "build-apu")
    for test in (test_four_packets_and_coalesced_completion, test_completion_before_next_process,
                 test_pause_gain_pitch_and_adpcm, test_stop_timeout_retains_ownership,
                 test_stop_timeout_late_completion,
                 test_halo_channel_reuse, test_static_buffer, test_driver_descriptor_retirement_and_pinning,
                 test_driver_pio_completion_interleaving):
        test(build)
        print(f"PASS {test.__name__}")
    print("Linked APU adapter checks passed; audible output and timing require device validation.")
