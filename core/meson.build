# Top-level Meson build for the core firmware.
#
# Two outputs from one source tree:
#   - core.ipod  : ARM cross-compile (the firmware that ships on device)
#   - core-sim   : host build, SDL-backed (for development on Linux/macOS)
#
# Pick the target with: meson setup build-hw  --cross-file cross/arm-none-eabi.txt -Dtarget=hw
#                       meson setup build-sim                                       -Dtarget=sim
#
# This file is intentionally minimal at the moment — subsystems land in
# subsequent PRs and will register themselves via subdir() calls below.

project(
  'core',
  'c',
  version: '0.0.1',
  license: 'Apache-2.0',
  default_options: [
    'c_std=gnu11',
    'warning_level=3',
    'werror=true',
    'b_lto=true',
    'optimization=s',          # -Os; size-optimized
  ],
)

target = get_option('target')
cc = meson.get_compiler('c')

# Warning flags that only exist on newer compilers go through
# get_supported_arguments(), which probes the ACTUAL compiler in use. This
# matters for -Wno-error= in particular: passing -Wno-error=<name-the-compiler-
# does-not-know> makes gcc emit "'-Werror=' argument ... is not valid", and
# under -Werror that is itself a hard error — so an unguarded flag added for
# gcc 16 would break the build on gcc 14. The toolchain floor lives in
# .tool-versions; this keeps older-but-supported toolchains working anyway.
optional_c_args = cc.get_supported_arguments([
  # gcc 16 added -Wunterminated-string-initialization, which fires on the
  # deliberate `static const char hex[16] = "0123456789ABCDEF"` lookup tables
  # in hal/hw/uart.c and hal/hw/lcd.c (exactly-sized char arrays, never used
  # as C strings) and broke the -Werror hw build outright on the 14->16
  # upgrade. The correct fix is __attribute__((nonstring)) on those two
  # arrays, which lands separately in the files that own them. Until then
  # this DEMOTES the diagnostic to a warning rather than silencing it
  # (-Wno-error=, not -Wno-), so the build goes through and the arrays stay
  # visible. REMOVE THIS once uart.c/lcd.c carry the attribute.
  # tests/meson.build passes the stronger per-target -Wno- form for the same
  # two files.
  '-Wno-error=unterminated-string-initialization',
])

# Common warning/safety flags
# (warning_level=3 already implies -Wall -Wextra -Wpedantic; we add specifics.)
common_c_args = optional_c_args + [
  '-ffunction-sections',
  '-fdata-sections',
  '-Wundef',
  '-Wshadow',
  # NOTE: -Wno-unused-parameter used to live here ("too noisy in driver stubs
  # during scaffold"). The scaffold is gone and the suppression was hiding
  # genuine forgot-to-use-the-argument bugs across all first-party code, so it
  # is no longer global. The vendored codec blocks below still list it in their
  # own suppression sets, which is where it belongs.
  #
  # Version-dependent suppressions live in optional_c_args above, which probes
  # the compiler before adding them.
]

common_link_args = [
  '-Wl,--gc-sections',
]

if target == 'hw'
  # ARM cross-build. Toolchain + ARM-specific flags live in
  # cross/arm-none-eabi.txt under [built-in options]; we add only the
  # freestanding-correctness flags that don't belong in the cross file.
  common_c_args += [
    '-fno-builtin',
    '-fno-common',
    '-fno-stack-protector',
    # Stack-depth guard. There is no stack probe and no guard page on this
    # target: the supervisor stack is 0x40017C00 growing down with only the
    # 1 KB banked IRQ stack above it, so one oversized frame silently corrupts
    # memory instead of faulting. 1024 bytes/frame is well above anything
    # legitimate here (the deepest real frames are a few hundred bytes of
    # dirent/sector scratch) and well below the available stack, so it catches
    # a new multi-KB local array at compile time. Raise it only with a note
    # about where the space came from.
  ] + cc.get_supported_arguments(['-Wframe-larger-than=1024'])
  add_project_arguments(common_c_args, language: 'c')
  add_project_link_arguments(common_link_args, language: 'c')
  message('Configured for hardware target (ARM, iPod 5G/5.5G)')

elif target == 'sim'
  # Host build. SDL2 provides LCD window + audio out + input.
  sdl2_dep = dependency('sdl2', required: true)
  add_project_arguments(common_c_args, language: 'c')
  add_project_link_arguments(common_link_args, language: 'c')
  message('Configured for sim target (host, SDL2)')

else
  error('Unknown target "@0@" (use -Dtarget=hw or -Dtarget=sim)'.format(target))
endif

subdir('hal')         # hardware abstraction layer (sim or hw backend)

if target == 'hw'
  # Bare-metal ARM build. boot/ + kernel/ are all that's freestanding-
  # clean today; codecs, audio engine, tagcache, and UI still pull in
  # host libc and land in subsequent PRs as they're brought up.
  subdir('boot')
  subdir('kernel')
  subdir('fs')
  subdir('lib')

  # FLAC decoder (dr_flac) built freestanding for the target: no libc — a
  # static arena backs allocation and our lib/mem.c provides memcpy/memset
  # (see codecs/dr_flac/flac.c's CORE_FREESTANDING block). Warnings are
  # suppressed and werror disabled for the vendored single-header source,
  # which we don't own; arena.c + mem.c are ours and stay under -Werror.
  codec_warn_suppress = [
    '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-variable',
    '-Wno-sign-compare', '-Wno-shadow', '-Wno-undef', '-Wno-cast-align',
    '-Wno-implicit-fallthrough', '-Wno-misleading-indentation',
  ]
  codec_inc = include_directories('codecs', 'codecs/dr_flac', 'codecs/dr_mp3', 'lib')
  flac_hw_lib = static_library(
    'core_codec_flac_hw',
    sources: ['codecs/dr_flac/flac.c'],
    c_args: ['-DCORE_FREESTANDING'] + codec_warn_suppress,
    include_directories: codec_inc,
    override_options: ['werror=false'],
  )

  # MP3 decoder (dr_mp3) built freestanding, same recipe as flac_hw_lib.
  # Unlike FLAC, MP3's synthesis filter is floating point, but dr_mp3 calls
  # no libm — the float ops lower to libgcc soft-float (__aeabi_*), already
  # linked via -lgcc below. Vendored source: warnings suppressed, werror off;
  # sign-compare/type-limits are extra for dr_mp3 (mirrors codecs/meson.build).
  mp3_hw_lib = static_library(
    'core_codec_mp3_hw',
    sources: ['codecs/dr_mp3/mp3.c'],
    c_args: ['-DCORE_FREESTANDING'] + codec_warn_suppress + [
      '-Wno-sign-compare', '-Wno-type-limits',
    ],
    include_directories: codec_inc,
    override_options: ['werror=false'],
  )

  # Read-ahead byte-source shim (ours, not vendored): collapses a decoder's
  # many tiny reads into a few large disk reads. Built freestanding so it pulls
  # lib/mem.h's memcpy (via CORE_FREESTANDING); stays under -Werror since we own
  # it and it's warning-clean.
  readahead_hw_lib = static_library(
    'core_readahead_hw',
    sources: ['codecs/readahead.c'],
    c_args: ['-DCORE_FREESTANDING'],
    include_directories: codec_inc,
  )

  # FLAC metadata reader (ours, not vendored): parses a FLAC's STREAMINFO +
  # VORBIS_COMMENT blocks up front so the UI can show real title/artist/album/
  # genre/track#/year + duration without decoding audio. Pulls bytes through a
  # decoder_source_t (same fat_src + read-ahead path as the streaming decoder).
  # Freestanding (no libc/libm/malloc — all buffers are stack/struct); reaches
  # decoder.h via a relative include (no extra dirs). Same flac_meta.c the host
  # flac_meta_test compiles; stays under -Werror since we own it.
  flac_meta_hw_lib = static_library(
    'core_flac_meta_hw',
    sources: ['codecs/flac_meta.c'],
    c_args: ['-DCORE_FREESTANDING'],
    include_directories: codec_inc,
  )

  # Anti-skip disk buffer (ours, not vendored): a MB-scale watermarked buffer
  # that reads ahead in bursts so the drive head parks between refills instead of
  # streaming continuously (the "constant arm movement" fix). Sits under the
  # read-ahead shim. Freestanding (CORE_FREESTANDING → lib/mem.h memcpy);
  # warning-clean under -Werror. Same diskbuf.c the host diskbuf_test compiles.
  diskbuf_hw_lib = static_library(
    'core_diskbuf_hw',
    sources: ['codecs/diskbuf.c'],
    c_args: ['-DCORE_FREESTANDING'],
    include_directories: codec_inc,
  )

  # Freestanding antialiased text renderer (ours): draws Nunito type from the
  # pre-rasterized atlas/*.h data into an RGB565 framebuffer with a gamma-correct
  # integer blend — no libc/libm/malloc. Same text.c the host text_test compiles;
  # links into core.elf so the on-device UI can draw real type. Uses only relative
  # includes (ui/atlas.h -> ../hal/hal.h), so no extra include dirs; stays
  # under -Werror since we own it and it's warning-clean.
  text_hw_lib = static_library(
    'core_text_hw',
    sources: ['ui/text.c'],
    c_args: ['-DCORE_FREESTANDING'],
  )

  # Freestanding nearest-neighbor RGB565 downscaler (ours): shrinks a loaded
  # folder.art to a list chip / album-detail cover when no dedicated folder.thm
  # is on disk. Pure integer math — no libc/libm/malloc, no allocation. Same
  # thumb.c the host thumb_test compiles; links into core.elf. Only relative
  # includes (thumb.h), so no extra include dirs; stays under -Werror.
  thumb_hw_lib = static_library(
    'core_thumb_hw',
    sources: ['ui/thumb.c'],
    c_args: ['-DCORE_FREESTANDING'],
  )

  # Full-screen "charging" battery view (ours): renders design_reference's
  # ChargingScreen into the RGB565 console framebuffer with fill_rect shapes +
  # the Nunito text renderer. Pure integer rendering — no libc/libm/malloc, no
  # hardware access. Relative includes only (ui/text.h, ../kernel/console.h,
  # ../hal/hal.h), so no extra include dirs; stays under -Werror.
  screen_charging_hw_lib = static_library(
    'core_screen_charging_hw',
    sources: ['ui/screen_charging.c'],
    c_args: ['-DCORE_FREESTANDING'],
  )

  # Freestanding incremental album-art thumbnail cache (ours): loads AT MOST one
  # album cover per pump() call and pre-shrinks it to a 22x22 list chip, spreading
  # the disk I/O across many main-loop passes so scrolling the album list never
  # stalls the audio decode/DMA loop. Uses thumb.c to downscale and fat32 to read;
  # no libc/libm/malloc, no allocation. Reaches fat32.h + thumb.h via relative
  # includes (no extra include dirs); stays under -Werror since we own it.
  artcache_hw_lib = static_library(
    'core_artcache_hw',
    sources: ['ui/artcache.c'],
    c_args: ['-DCORE_FREESTANDING'],
  )

  # Settings subsystem (ours): a data-driven settings_t model + a matching
  # renderer for the Settings screens (design_reference menus.jsx + ThemePicker).
  # settings.c is pure model logic (no framebuffer/hardware — the SAME source the
  # host settings_test compiles); screen_settings.c draws it via the Nunito text
  # renderer + console fill_rect. main.c owns the settings_t state + navigation
  # and calls these generically. No libc/libm/malloc; relative includes only
  # (settings.h, ui/text.h, ../kernel/console.h, ../hal/hal.h); stays -Werror.
  settings_hw_lib = static_library(
    'core_settings_hw',
    sources: ['ui/settings.c', 'ui/screen_settings.c', 'ui/palette.c'],
    c_args: ['-DCORE_FREESTANDING'],
  )

  # Background streaming playback engine (ours): the FLAC/MP3 decode path, the
  # SPSC PCM ring feeding the DMA DAC, the fat32 byte-source + read-ahead glue,
  # album art, and the queue/auto-advance player — extracted out of kernel/main.c
  # into core/player/. Freestanding (its struct-copy lowers to lib/mem.c's memcpy,
  # resolved at the final link against mem_dep). Needs the hal include dir for
  # hw/*.h + hal.h; codec/kernel/fs headers are reached via relative includes.
  player_hw_lib = static_library(
    'core_player_hw',
    sources: ['player/player.c'],
    include_directories: include_directories('hal', 'codecs', 'codecs/dr_flac', 'codecs/dr_mp3'),
  )

  core_elf = executable(
    'core.elf',
    boot_sources + kernel_sources + fs_sources + files('codecs/arena.c'),
    dependencies: [hal_dep, mem_dep],   # hal/hw: SER0 UART; mem: freestanding memcpy/…
    link_with: [flac_hw_lib, mp3_hw_lib, flac_meta_hw_lib, readahead_hw_lib, diskbuf_hw_lib, text_hw_lib, thumb_hw_lib, screen_charging_hw_lib, artcache_hw_lib, settings_hw_lib, player_hw_lib],
    include_directories: codec_inc,
    link_args: [
      '-Wl,-T,' + (meson.current_source_dir() / 'boot' / 'linker.ld'),
      '-Wl,-Map=core.map',
      # libgcc: the ARM7TDMI has no hardware divide, so integer division
      # (fat32 sector math, playback timing) needs the compiler runtime's
      # __aeabi_*divmod helpers. This is the compiler runtime, NOT libc.
      '-lgcc',
      # The pcm_ring SPSC indices use __atomic acquire/release ordering.
      # On ARMv4T (no barrier instruction) GCC lowers the fence to a
      # __sync_synchronize libcall; sync-none selects its no-op form. That
      # is correct HERE: the ring's producer (foreground pump) and consumer
      # (DMA ISR) run on the SAME core — ARM7TDMI is in-order and strongly
      # ordered, and the DMA engine reads the PCM buffer, never the indices,
      # so no cross-observer hardware fence is needed. The __atomic builtins
      # still emit the compiler-ordering barrier we actually rely on. Absent
      # this spec the stub links anyway but warns on every build.
      #
      # -specs=sync-none.specs ALONE does not take effect here: the spec body is
      # guarded by `%{!nostdlib|lgcc: --defsym=...}`, and we link with -nostdlib
      # while -lgcc is an input FILE rather than a spec-visible switch, so the
      # guard never fires and libgcc's warning-carrying _sync_synchronize.o was
      # still pulled in ("This implementation of __sync_synchronize is a stub
      # with no effect"). Passing the same --defsym the spec would have passed
      # states the intent unconditionally and stops the archive member (and its
      # .gnu.warning section) from ever being needed. The spec is kept so the
      # declaration is discoverable by the usual name.
      '-specs=sync-none.specs',
      '-Wl,--defsym=__sync_synchronize=__sync_synchronize_none',
      # Bare-metal image: .text/.data/.bss share one RWX SDRAM segment by
      # design (there is no MMU and no loader to honour page permissions), so
      # ld's hardening warning about it is noise, not a finding.
      '-Wl,--no-warn-rwx-segments',
    ],
    link_depends: boot_linker_script,
  )

  # Flat binary derived from the ELF — the host-side `core firmware pack`
  # wraps this in the .ipod transport format. Both products are byproducts
  # of `make hw`; the Makefile's `ipod` target does the .ipod wrap.
  objcopy = find_program('objcopy')
  custom_target(
    'core.bin',
    input: core_elf,
    output: 'core.bin',
    command: [objcopy, '-O', 'binary', '@INPUT@', '@OUTPUT@'],
    build_by_default: true,
  )

  # Post-link size / static-RAM budget gate. `ninja` runs this every time
  # core.elf relinks: it prints the text/data/bss breakdown and FAILS the build
  # when a section is past the budget documented in the script. Without it a new
  # MB-scale static buffer grows .bss silently — .bss is already ~10.1 MB inside
  # a 32 MB SDRAM window, and nothing else in the tree reports the number.
  # The output file is just a stamp holding the printed report.
  size_prog = find_program('arm-none-eabi-size', required: true)
  custom_target(
    'core-size-budget',
    input: core_elf,
    output: 'core.size',
    command: [
      find_program('sh'),
      meson.current_source_dir() / 'tests' / 'scripts' / 'check_size.sh',
      '@INPUT@', size_prog.full_path(), '@OUTPUT@',
    ],
    build_by_default: true,
  )

elif target == 'sim'
  subdir('codecs')      # vendored audio decoders (FLAC + MP3)
  subdir('tests')       # host unit tests for the freestanding firmware code
endif

# Future subsystems land here as they're written:
#
#   target == 'hw':  fs/ (FAT32), then codecs/apps once freestanding-clean
#   either:          kernel/ scheduler, IRQ table
