# Codecs

Vendored audio decoders, plus the unified ABI they conform to.

## ABI

[`decoder.h`](decoder.h) defines `decoder_t` and `decoder_ops_t` — the
contract every codec wrapper implements. The audio engine doesn't know
or care which codec is playing; it only sees `decoder_t`.

Output format is always 16-bit signed interleaved PCM. Codecs that
decode at higher bit depth (e.g. 24-bit FLAC) downconvert in their
wrapper.

## Freestanding

Both shipped decoders build freestanding for the device
(`-DCORE_FREESTANDING`): **no libc, no malloc, no libm**.
`DR_FLAC_NO_STDIO` / `DR_MP3_NO_STDIO` remove the file paths;
allocation always goes through explicitly-passed callbacks backed by a
static arena, so dr_flac's default `MALLOC`/`REALLOC`/`FREE` are defined to
NULL/no-op and are never reached; assertions compile out; memory ops route to
`lib/mem.c`.

FLAC's decode path is **pure integer**. MP3's is not — dr_mp3's synthesis
filter is plain `float` arithmetic, which on this FPU-less ARM7TDMI the
compiler lowers to libgcc's soft-float runtime (`__aeabi_fmul` and friends).
It links and it is bit-exact under the KAT, but it cannot keep the PCM ring
fed in real time, which is why MP3 is built but parked on the device. See
[`../README.md`](../README.md), "Audio path".

## FLAC: CRC is ON, and that is a seek decision

`DR_FLAC_NO_CRC` is **not** defined. It used to be, on what looked like
sound reasoning: dr_flac CRCs every decoded frame on top of the decode, and
we act on the result nowhere — a failed frame comes back as zero frames,
which `flac_decode` reports as end-of-stream exactly like a genuine EOS. So
the check cost cycles and bought nothing.

The reasoning was sound and the conclusion was still wrong, because
`DR_FLAC_NO_CRC` **also compiles out dr_flac's binary-search seek**:

```c
#if !defined(DR_FLAC_NO_CRC)
    if (!wasSuccessful && ...) { ...binary_search(...) }
#endif
```

That guard is load-bearing, not incidental: a binary search lands on an
arbitrary byte and has to decide whether it is looking at a real frame
header or at audio data that merely resembles a sync code — the header CRC
is what settles it.

With CRC off, and a library whose files carry **no SEEKTABLE** (verified:
these FLACs hold STREAMINFO + PICTURE + VORBIS_COMMENT + PADDING and nothing
else), every seek fell through to brute force — decoding from the start of
the track. Measured on device 2026-07-27: resuming ~30 s into a track cost
5.1 s of an 8.4 s cold boot, and the same cost applied to every manual
scrub.

Trading some per-frame decode margin for O(log n) seeks is the right way
round. The margin is visible on **Settings → Boot Details** (DECODE, against
the 22676 µs/kframe real-time budget at 44.1 kHz); if it ever gets tight the
fix is a seek table in the files, not brute-force seeking. The post-fix seek
timing has not yet been re-measured on the device.

## Codec status

| Format | Lib       | Status         | License |
|--------|-----------|----------------|---------|
| FLAC   | dr_flac   | ✅ shipping on device | Public domain (Unlicense) / MIT-0 |
| MP3    | dr_mp3    | ⏸ wrapped + KAT, parked (too slow — see below) | Public domain (Unlicense) / MIT-0 |
| AAC    | (TBD)     | TODO           | TBD — Helix AAC (RPSL) is the leading candidate |
| ALAC   | Apple ALAC| TODO           | Apache-2.0 |
| Vorbis | Tremor    | TODO           | BSD-2 |
| Opus   | libopus   | TODO           | BSD-2 |
| WAV    | (own)     | TODO           | (Apache-2.0, ours) |

Note on MP3 / AAC: PLAN.md targeted Helix MP3 and Helix AAC for
absolute decode performance on ARM7. We picked dr_mp3 first because
its single-header vendoring matches dr_flac's pattern and exercises
the ABI under a different (lossy, no total_frames at open) shape with
minimal vendoring complexity.

That question is now answered: dr_mp3 does not make real time on this CPU
(soft-float synthesis, see "Freestanding"), so `kernel/main.c` sets
`CORE_ENABLE_MP3 0` and `.mp3` is not surfaced in the browser at all. A
fixed-point decoder — Helix MP3 is still the leading candidate — is what
would unpark it. Swapping one in is one wrapper file; the ABI doesn't
change.

## Adding a new codec

1. Create `codecs/<codec>/` with:
   - `LICENSE` — preserve the upstream license verbatim.
   - `vendor.sh` — script that re-fetches the upstream source, with
     the URL and pinned version recorded.
   - `<lib>.h` / `<lib>.c` (or single-header) — the upstream source.
   - `<codec>.h` / `<codec>.c` — our wrapper, exposing
     `<codec>_decoder_ops()`.
2. Wire into `codecs/meson.build` as a new `static_library` and
   `declare_dependency`. Add to the `codecs_dep` aggregate.
3. Add a KAT to `tests/codec_kat.c` and a fixture to
   `tests/codec-vectors/` (regenerate via
   `tests/scripts/gen_codec_vectors.sh`).
4. Update this table.

## Running the KAT

The host build wires the test into Meson's runner:

```bash
cd core
make sim         # or: meson setup build-sim -Dtarget=sim && ninja -C build-sim
meson test -C build-sim       # runs every codec's KAT
```

Or directly:

```bash
./build-sim/tests/codec_kat ./tests/codec-vectors
# expected:
#   OK: flac decoded 44100 frames, 176400 bytes bit-exact
#   OK: mp3 decoded 44100 frames, 176400 bytes bit-exact
```

## Test vectors

All fixtures in `core/tests/codec-vectors/`. Generated by
`core/tests/scripts/gen_codec_vectors.sh`; regenerate only on a pinned
host (libm `sin()` is not bit-stable across platforms — the .pcm
output of the Python generator wobbles by ≤1 LSB across glibc / musl /
macOS, so the committed fixture is the source of truth).

| File                                            | Source                                              |
|-------------------------------------------------|-----------------------------------------------------|
| `sine_440hz_1s_44k_s16_stereo.pcm`              | Synthetic 440 Hz sine, 1 s, 44.1 kHz, s16, stereo, ±16000 |
| `sine_440hz_1s_44k_s16_stereo.flac`             | Above PCM piped through `flac` reference encoder    |
| `sine_440hz_1s_44k_s16_stereo_128k.mp3`         | Above PCM piped through `ffmpeg -c:a libmp3lame -b:a 128k` |
| `sine_440hz_1s_44k_s16_stereo_128k.mp3.ref.pcm` | dr_mp3's decoded output captured at fixture time    |

For lossless codecs (FLAC) the reference is the original input PCM —
round-trip must be byte-exact. For lossy codecs (MP3) the reference
is what dr_mp3 produces *now*; the KAT catches any regression in
dr_mp3 or our wrapper that changes output. It does not validate
absolute decoder correctness against external truth (different MP3
decoders aren't bit-stable; that's not a thing the format guarantees).
