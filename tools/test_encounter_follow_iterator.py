#!/usr/bin/env python3
"""Exercise Halo's actual follow-target collection with a 24-byte iterator."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--halo-root", type=Path, default=Path("/mnt/g/dev/halo"))
    args = parser.parse_args()
    source = (args.halo_root / "src/halo/ai/encounters.c").read_text()
    start = source.index("void encounter_update_follow(int encounter_handle)")
    end = source.index("\nhave_targets:", start)
    body = source[start:end] + "\nhave_targets:\n observed = targets[0];\n}\n"
    # The original iterator stores three selector words and three actor words.
    # Use one valid actor; its final advance writes NONE at iterator word four.
    harness = r'''
#include <assert.h>
#include <stddef.h>
#define MAXIMUM_SQUADS_PER_ENCOUNTER 64
typedef struct { float x, y, z; } vector3_t;
typedef struct { int unused; } encounter_definition;
typedef struct { int unused; } data_iter_t;
typedef struct { int unit_handle; } player_data_t;
typedef struct { char pad[0x18]; int field_018; } actor_t;
static char encounter[0x68];
static actor_t actor;
static int observed = -1;
static void *encounter_data, *player_data;
static void *datum_get(void *data, int handle) { return encounter; }
static void *global_scenario_get(void) { return encounter; }
static void *tag_block_get_element(void *block, int index, int size) { return 0; }
static void data_iterator_new(data_iter_t *iter, void *data) { }
static player_data_t *data_iterator_next(data_iter_t *iter) { return 0; }
static void *object_try_and_get_and_verify_type(int handle, int mask) { return 0; }
static void ai_index_actor_iterator_new(int reference, void *buffer)
{
    int *iter = buffer;
    iter[0] = reference; iter[1] = -1; iter[2] = -1;
    iter[3] = reference; iter[4] = -1; iter[5] = 0x42;
}
static actor_t *ai_index_actor_iterator_next(void *buffer)
{
    int *iter = buffer;
    iter[4] = iter[5];
    if (iter[5] == -1) return 0;
    iter[5] = -1;
    return &actor;
}
'''
    harness += body
    harness += r'''
int main(void)
{
    *(short *)(encounter + 0x62) = 3;
    *(int *)(encounter + 0x64) = 1;
    actor.field_018 = 0x12345678;
    encounter_update_follow(0);
    assert(observed == actor.field_018);
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="halo-follow-iterator-") as directory:
        root = Path(directory)
        (root / "follow.c").write_text(harness)
        subprocess.run(["cc", "-std=c89", "-O1", "-g", "-fsanitize=address",
                        "-fno-omit-frame-pointer", str(root / "follow.c"),
                        "-o", str(root / "follow")], check=True)
        environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
        subprocess.run([str(root / "follow")], check=True, env=environment)
    print("PASS: follow-target collection preserves a valid actor unit handle")


if __name__ == "__main__":
    main()
