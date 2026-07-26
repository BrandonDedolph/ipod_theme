# Test harness: codec known-answer tests.
#
# Builds codec_kat as a host executable; the fixture directory is
# passed at runtime. `meson test` picks them up.

if target == 'sim'
  # libm needed only on platforms where it's a separate library;
  # macOS and most BSDs link math via libSystem and reject -lm.
  cc = meson.get_compiler('c')
  m_dep = cc.find_library('m', required: false)

  codec_kat = executable(
    'codec_kat',
    sources: ['codec_kat.c'],
    dependencies: [codecs_dep, m_dep],
  )

  vectors_dir = meson.current_source_dir() / 'codec-vectors'
  # codec_kat runs every vendored codec's KAT in one binary; one
  # meson test entry covers all of them. Failures print which codec
  # failed via the [codec] label in stderr.
  test('codec-kat', codec_kat,
       args: [vectors_dir],
       suite: 'unit')

  # flac-freestanding: the STREAMING FLAC decode path used on hw — open a
  # FLAC through flac_open_stream (a decoder_source_t pull source) with the
  # static arena instead of libc malloc, decode every frame, bit-compare to
  # the reference PCM, and assert the arena never OOM'd. Same flac.c + arena.c
  # the ARM build compiles; only the byte source differs (memory vs disk).
  flac_fs_test = executable(
    'flac_freestanding_test',
    sources: ['codecs/flac_freestanding_test.c', '../codecs/arena.c'],
    dependencies: [flac_dep],
    include_directories: include_directories('../codecs', '../codecs/dr_flac'),
  )
  test('flac-freestanding', flac_fs_test, args: [vectors_dir], suite: 'unit')

  # mp3-freestanding: the STREAMING MP3 decode path used on hw — mirror of
  # flac-freestanding. Open the MP3 through mp3_open_stream (a decoder_source_t
  # pull source) with the static arena instead of libc malloc, decode every
  # frame, bit-compare to the reference PCM (dr_mp3's own captured output, the
  # KAT truth for a lossy codec), and assert the arena never OOM'd. Same mp3.c +
  # arena.c the ARM build compiles; only the byte source differs (memory vs disk).
  mp3_fs_test = executable(
    'mp3_freestanding_test',
    sources: ['codecs/mp3_freestanding_test.c', '../codecs/arena.c'],
    dependencies: [mp3_dep],
    include_directories: include_directories('../codecs', '../codecs/dr_mp3'),
  )
  test('mp3-freestanding', mp3_fs_test, args: [vectors_dir], suite: 'unit')

  # readahead: the block-buffering shim (codecs/readahead.c) that collapses a
  # decoder's many tiny reads into a few large backing reads — the fix for the
  # ~27 s on-device MP3 startup. Backed by an instrumented in-RAM source that
  # counts backing reads, so the test proves both byte-exactness and coalescing.
  # Same readahead.c the ARM build compiles.
  readahead_test = executable(
    'readahead_test',
    sources: ['codecs/readahead_test.c', '../codecs/readahead.c'],
    include_directories: include_directories('../codecs'),
  )
  test('readahead', readahead_test, suite: 'unit')

  # flac-meta: the FLAC metadata reader (codecs/flac_meta.c) — the SAME source
  # the ARM build links into core.elf. Backs a decoder_source_t with an in-RAM
  # buffer and asserts: the committed KAT vector parses to sample_rate 44100 /
  # 1 s duration (its VORBIS_COMMENT has no tags), a synthetic FLAC we build in
  # RAM round-trips a full tag set (title/artist/ALBUMARTIST fallback/album/
  # genre/"7/12" track/"2021-05-01" date) with UTF-8 stripped, and malformed /
  # hostile inputs return -1 or bail without overrunning. Takes the fixture dir
  # as argv[1] to find the KAT flac.
  flac_meta_test = executable(
    'flac_meta_test',
    sources: ['codecs/flac_meta_test.c', '../codecs/flac_meta.c'],
    include_directories: include_directories('../codecs'),
  )
  test('flac-meta', flac_meta_test, args: [vectors_dir], suite: 'unit')

  # diskbuf: the MB-scale anti-skip buffer (codecs/diskbuf.c) that reads ahead in
  # bursts so the drive head parks between refills (the "constant arm movement"
  # fix). Backed by the same instrumented in-RAM source; asserts byte-exactness
  # across reads/seeks AND that the pump is bursty (fills to high, idles, refills
  # below low). Same diskbuf.c the ARM build compiles.
  diskbuf_test = executable(
    'diskbuf_test',
    sources: ['codecs/diskbuf_test.c', '../codecs/diskbuf.c'],
    include_directories: include_directories('../codecs'),
  )
  test('diskbuf', diskbuf_test, suite: 'unit')

  # text: the freestanding antialiased text renderer (core/ui/text.c) — the
  # SAME source the ARM build links into core.elf. Renders "Ag" into a small
  # RGB565 buffer over a known bg/ink and asserts width is positive+additive
  # and matches the draw advance, that interior pixels blended toward the ink
  # (incl. partial-coverage greys, proving the gamma blend ran), that pixels
  # outside coverage are untouched, and that drawing off every edge writes no
  # OOB (sentinel guard cells bracket the framebuffer). No MMIO, plain cc.
  text_test = executable(
    'text_test',
    sources: ['ui/text_test.c', '../ui/text.c'],
    include_directories: include_directories('../ui'),
  )
  test('text', text_test, suite: 'unit')

  # thumb: the freestanding nearest-neighbor RGB565 downscaler (core/ui/thumb.c)
  # — the SAME source the ARM build links into core.elf. Shrinks a loaded
  # folder.art to a list chip / album-detail cover when no folder.thm is present.
  # Asserts an exact identity copy (dst == src dims), a hand-worked 4x4->2x2 pixel
  # selection, in-bounds source reads + no OOB writes for a non-integer ratio
  # (120->22, sentinel-guarded dst), and the zero/negative-dim no-op. Plain cc.
  thumb_test = executable(
    'thumb_test',
    sources: ['ui/thumb_test.c', '../ui/thumb.c'],
    include_directories: include_directories('../ui'),
  )
  test('thumb', thumb_test, suite: 'unit')

  # settings: the Settings state model (core/ui/settings.c) — the SAME source the
  # ARM build links into core.elf. Pure model logic (no framebuffer/hardware), so
  # it compiles with plain cc. Asserts defaults, that activate() toggles Shuffle
  # and cycles Repeat OFF->ALL->ONE->OFF, that adjust() clamps Volume/Bass and
  # Display Brightness to range, that Backlight cycles its discrete list, and that
  # Root rows return the right ENTER_*/RESET action codes plus value/kind report.
  settings_test = executable(
    'settings_test',
    sources: ['ui/settings_test.c', '../ui/settings.c'],
    include_directories: include_directories('../ui'),
  )
  test('settings', settings_test, suite: 'unit')

  tag_mp3_test = executable(
    'tag_mp3_test',
    sources: ['tag_mp3_test.c'],
    dependencies: [codecs_dep],
  )
  test('tag-mp3', tag_mp3_test, suite: 'unit')

  # hw-mmio golden-trace tests: compile the freestanding hw drivers
  # host-side with -DMMIO_MOCK so the recording fake bus
  # (hw_mmio/mmio_mock.c) captures the exact register-access grammar,
  # then assert it. This is the only automated check of the BCM/LCD
  # transaction stream — clicky models no BCM, the device needs a logic
  # analyzer. The drivers compile unchanged (freestanding: stdint +
  # hal.h + the mmio.h seam only); in the firmware build the same seam
  # is a static-inline volatile deref, so core.bin is byte-identical.
  hw_mmio_inc = include_directories('../hal/hw', '../hal', 'hw_mmio')

  uart_trace_test = executable(
    'uart_trace_test',
    sources: ['hw_mmio/uart_trace_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/uart.c'],
    include_directories: hw_mmio_inc,
    c_args: ['-DMMIO_MOCK', '-Wno-unterminated-string-initialization'],
  )
  test('hw-uart-trace', uart_trace_test, suite: 'unit')

  lcd_trace_test = executable(
    'lcd_trace_test',
    sources: ['hw_mmio/lcd_trace_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/lcd.c'],
    include_directories: hw_mmio_inc,
    c_args: ['-DMMIO_MOCK', '-Wno-unterminated-string-initialization'],
  )
  test('hw-lcd-trace', lcd_trace_test, suite: 'unit')

  # Static header<->doc consistency: pp5022.h addresses vs the
  # docs/hw/ reference + internal stride/window invariants. Pure source
  # parsing (no ARM build needed), so it runs in the common test path.
  # Catches the dropped-zero and byte-vs-word-stride bug classes. The
  # ELF-layout half (crt0/linker asserts, needs core.elf) runs via
  # `make verify-hw`.
  py = find_program('python3', required: false)
  if py.found()
    test('hw-doc-consistency', py,
         args: [meson.current_source_dir() / 'scripts' / 'check_hw_consistency.py'],
         suite: 'unit')
  endif

  # hw-sched: the cooperative scheduler's task-table + round-robin
  # policy, compiled host-side with -DSCHED_TEST against inert stubs for
  # the two asm symbols (switch_context / task_trampoline). Proves task
  # add/bounds, round-robin selection, done-task skipping, and the
  # single-runnable no-switch case. The context-switch asm itself is
  # exercised end-to-end in the clicky emulator, not here.
  sched_test = executable(
    'sched_test',
    sources: ['hw_sched/sched_test.c',
              'hw_sched/sched_test_mocks.c',
              '../kernel/sched.c'],
    include_directories: include_directories('../kernel'),
    c_args: ['-DSCHED_TEST'],
  )
  test('hw-sched', sched_test, suite: 'unit')

  # hw-timer: the interrupt dispatcher + 100 Hz timer tick, compiled
  # host-side with -DMMIO_MOCK against the recording fake bus. Asserts
  # timer_init's exact register grammar (arm CFG + unmask), that
  # timer_tick_isr acks by reading TIMER1_VAL and advances the counter,
  # that irq_dispatch only ticks when CPU_INT_STAT shows TIMER1 pending,
  # and the sleep_ms tick math (a sched_yield stub advances the mocked
  # tick so the cooperative wait terminates). The IRQ entry asm + real
  # interrupt delivery are exercised end-to-end in clicky.
  timer_test = executable(
    'timer_test',
    sources: ['hw_timer/timer_test.c',
              'hw_timer/timer_test_stubs.c',
              '../kernel/timer.c',
              '../kernel/irq.c',
              'hw_mmio/mmio_mock.c'],
    include_directories: include_directories('../hal', '../hal/hw', '../kernel', 'hw_mmio'),
    c_args: ['-DMMIO_MOCK'],
  )
  test('hw-timer', timer_test, suite: 'unit')

  # hw-clock: the PLL/clock-boost driver's register grammar (arm CFG +
  # lock poll + timing), the bounded lock-spin (clicky never sets the PLL
  # lock bit), and the boost refcount edges — host-side under -DMMIO_MOCK.
  clock_test = executable(
    'clock_test',
    sources: ['hw_clock/clock_test.c',
              '../kernel/clock.c',
              'hw_mmio/mmio_mock.c'],
    include_directories: include_directories('../hal', '../hal/hw', '../kernel', 'hw_mmio'),
    c_args: ['-DMMIO_MOCK'],
  )
  test('hw-clock', clock_test, suite: 'unit')

  # hw-audio: the first-sound driver chain (I2C controller -> WM8758
  # codec -> I2S serializer), host-side under -DMMIO_MOCK. The only
  # automated check of the audio register grammar — clicky models no
  # I2C/I2S/DAC and the device needs a logic analyzer; sound itself is
  # the on-device proof. Asserts i2c_init/i2c_send grammar + guards, the
  # 30-write wm8758_init with correct 9-bit framing / DAC route / unmute-
  # last ordering, i2s_init clock+FIFO grammar, and the polled write
  # (TXFree poll + packed word, plus the no-drain bail).
  audio_trace_test = executable(
    'audio_trace_test',
    sources: ['hw_audio/audio_trace_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/i2c.c',
              '../hal/hw/wm8758.c',
              '../hal/hw/i2s.c',
              '../hal/hw/dma.c',
              '../hal/hw/ata.c'],
    include_directories: hw_mmio_inc,
    c_args: ['-DMMIO_MOCK'],
  )
  test('hw-audio', audio_trace_test, suite: 'unit')

  # fat32: the read-only FAT32 reader, host-tested against a synthetic
  # image (BytesPerSector 2048 like the stock iPod 80 GB drive, plus a
  # file spanning two clusters) generated by make_fat32_image.py. Portable
  # logic, no MMIO — plain cc. Verifies mount, the 8.3 lookup (hit/miss,
  # case-insensitive), and full + partial file reads.
  if py.found()
    fat32_img = custom_target('fat32-test-image',
      output: 'fat32_test.img',
      command: [py, files('scripts' / 'make_fat32_image.py'), '@OUTPUT@'])
    fat32_test = executable('fat32_test',
      sources: ['fs/fat32_test.c', '../fs/fat32.c'],
      include_directories: include_directories('../fs'))
    test('fat32', fat32_test, args: [fat32_img], suite: 'unit')
  endif

  # pcm-ring: the SPSC PCM ring that decouples the streaming disk pump
  # (producer) from the DMA-completion ISR (consumer). Portable logic, no
  # MMIO — verifies free/fill accounting, over-write / under-read clamping,
  # the power-of-two wrap, and end-to-end FIFO byte integrity across many
  # wrap cycles.
  pcm_ring_test = executable(
    'pcm_ring_test',
    sources: ['kernel/pcm_ring_test.c', '../kernel/pcm_ring.c'],
    include_directories: include_directories('../kernel'),
  )
  test('pcm-ring', pcm_ring_test, suite: 'unit')

  # config: settings persistence (kernel/config.c) — the ONLY code in the
  # firmware that writes to the user's disk, so this is the highest-stakes
  # test in the suite. Four layers over an in-RAM FAT32 volume with
  # BytesPerSector 2048 at a non-zero partition base (the geometry that makes
  # the right and wrong LBA formulas differ):
  #   1. fat32_file_lba pins the absolute LBA to
  #      part_lba + cluster_fs_sector * sec_ratio, and explicitly asserts it
  #      is NOT the sec_ratio-less value the design doc proposed — which on
  #      the stock volume lands inside the FAT.
  #   2. the record codec: round-trip, and rejection of bad magic / version /
  #      length / a flipped payload, header or padding bit / all-zeros /
  #      garbage, plus range clamping of a CRC-valid but insane record.
  #   3. two-slot selection through config_load: newer seq wins, a torn or
  #      garbage slot loses to a good one, seq WRAPAROUND resolves correctly,
  #      both-bad keeps defaults, absent/short file disables writing.
  #   4. the write grammar, host-side under -DMMIO_MOCK: the exact
  #      WRITE SECTORS 0x30 / task-file / DRQ-out / FLUSH CACHE 0xE7 sequence
  #      ata.c emits, at the resolved LBA, with slot ALTERNATION asserted —
  #      plus that a refused save emits ZERO bus events.
  # Links the real ata.c, so the trace is the driver's, not a re-description
  # of it. What it cannot prove is that the drive obeys: see the on-device
  # bring-up procedure at the top of kernel/config.c.
  config_test = executable(
    'config_test',
    sources: ['kernel/config_test.c',
              '../kernel/config.c',
              '../fs/fat32.c',
              '../hal/hw/ata.c',
              'hw_mmio/mmio_mock.c'],
    include_directories: [hw_mmio_inc,
                          include_directories('../kernel', '../fs', '../ui')],
    c_args: ['-DMMIO_MOCK'],
  )
  if py.found()
    # 5. Host/device format agreement: the fixture is produced by the REAL
    # host tool (tools/make_config.py --emit), and the test decodes it with
    # the firmware's own config_decode(). The two encoders are independent
    # implementations of one spec — offsets, field order, signedness, CRC
    # polynomial and coverage — so without this they can drift and a freshly
    # created CORECFG.DAT would be silently rejected on device, with the only
    # symptom being "settings never persist" on a user's iPod.
    corecfg_fixture = custom_target('corecfg-fixture',
      output: 'corecfg_golden.bin',
      command: [py, files('..' / '..' / 'tools' / 'make_config.py'),
                '--emit', '@OUTPUT@'])
    test('config', config_test, args: [corecfg_fixture], suite: 'unit')
  else
    test('config', config_test, suite: 'unit')
  endif

  # hw-console: the on-screen debug console (framebuffer + 8x8 hex font).
  # Pure rendering logic, no MMIO — verifies glyph output pixel-for-pixel.
  console_test = executable(
    'console_test',
    sources: ['hw_console/console_test.c', '../kernel/console.c'],
    include_directories: include_directories('../kernel', '../hal'),
  )
  test('hw-console', console_test, suite: 'unit')

  # hw-clickwheel: the click-wheel/button/hold decode, host-side under
  # -DMMIO_MOCK. The status-word decode is pure integer math, so the mock
  # bus feeds synthetic 32-bit words (CLICKWHEEL_DATA) and a scripted hold
  # GPIO into the real clickwheel_poll() and asserts button decode, the
  # sensitivity-gated wheel delta (incl. sub-threshold accumulation and
  # wrap through 0x5F<->0x00), the valid-gate rejection, and the hold edge.
  clickwheel_test = executable(
    'clickwheel_test',
    sources: ['hw_clickwheel/clickwheel_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/clickwheel.c'],
    include_directories: hw_mmio_inc,
    c_args: ['-DMMIO_MOCK'],
  )
  test('hw-clickwheel', clickwheel_test, suite: 'unit')

  # hw-lcd-present: lcd_present() streams a caller framebuffer over the
  # BCM (same path as lcd_fill) — asserts the exact 38400-word grammar
  # and pixel packing against the recording mock bus.
  lcd_present_test = executable(
    'lcd_present_test',
    sources: ['hw_mmio/lcd_present_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/lcd.c'],
    include_directories: include_directories('../hal', '../hal/hw', 'hw_mmio'),
    c_args: ['-DMMIO_MOCK', '-Wno-unterminated-string-initialization'],
  )
  test('hw-lcd-present', lcd_present_test, suite: 'unit')

  # hw-backlight: the GPIO-bit-banged backlight dimmer (hal/hw/backlight.c)
  # host-side under -DMMIO_MOCK. The backlight has no status register — it's
  # a bare GPIO charge-pump dimmer — so this asserts the exact ordered write
  # grammar of backlight_init (route + power + 16->32 walk + LED on),
  # backlight_set(0) (single atomic LED-off), and backlight_set(MAX) (LED-on
  # + walk to full), each op a single write to the +0x800 atomic set/clear
  # alias. Pulse widths are untraced busy-loops (device-gated timing).
  backlight_trace_test = executable(
    'backlight_trace_test',
    sources: ['hw_mmio/backlight_trace_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/backlight.c'],
    include_directories: hw_mmio_inc,
    c_args: ['-DMMIO_MOCK'],
  )
  test('hw-backlight', backlight_trace_test, suite: 'unit')

  # hw-battery: the battery gauge (PCF50605 PMU ADC over I2C) + power-state
  # (GPIO input bits), host-side under -DMMIO_MOCK. There is no clicky PMU
  # model and the device needs a logic analyzer, so this is the only
  # automated check of the battery read grammar. It exercises the new
  # i2c.c register-pointer READ path end-to-end: asserts battery_millivolts
  # emits the exact channel-select write (ADCC1=0x05) + 2-byte result read
  # (ADCS1/ADCS2) transaction stream and returns the seeded raw's mV, and
  # that power_is_external/charging read the right GPIO input reg/width and
  # decode the right bit + polarity. Scaling + %-curve are DEVICE-GATED
  # (calibrate against the real cell); the test pins the transaction shape,
  # not the on-cell accuracy.
  battery_trace_test = executable(
    'battery_trace_test',
    sources: ['hw_mmio/battery_trace_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/battery.c',
              '../hal/hw/i2c.c'],
    include_directories: hw_mmio_inc,
    c_args: ['-DMMIO_MOCK'],
  )
  test('hw-battery', battery_trace_test, suite: 'unit')

  # hw-volume: the WM8758 output-volume HAL (hal/hw/volume.c) over the
  # reused i2c.c write path, host-side under -DMMIO_MOCK. Volume is not
  # modelled by clicky and the device needs a logic analyzer, so this is
  # the automated check of the percent->register policy: the PURE
  # percent->OUT1VOL mapping (monotonic gain, clamping, 0%=mute,
  # 100%=0 dB/0x39 never +6 dB) plus hal_volume_set's L-then-R(+VU)
  # two-write codec grammar and hal_volume_get round-trip.
  volume_trace_test = executable(
    'volume_trace_test',
    sources: ['hw_mmio/volume_trace_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/volume.c',
              '../hal/hw/i2c.c'],
    include_directories: hw_mmio_inc,
    c_args: ['-DMMIO_MOCK'],
  )
  test('hw-volume', volume_trace_test, suite: 'unit')
endif
