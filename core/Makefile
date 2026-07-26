# Convenience wrapper around Meson + Ninja.
#
# This is the human entry point. Real build logic lives in meson.build.
# The Go CLI (`core build hw` / `core build sim`) is a thin wrapper around
# these same targets — they should stay equivalent.

BUILD_HW  := build-hw
BUILD_SIM := build-sim

.PHONY: help hw ipod sim clean realclean reconfigure-hw reconfigure-sim verify-hw

help:
	@echo "Targets:"
	@echo "  make hw           Build the firmware ELF + flat binary (ARM, iPod 5G/5.5G)"
	@echo "  make ipod         Build hw, then wrap as .ipod transport image"
	@echo "  make sim          Build the host simulator (SDL2)"
	@echo "  make clean        Remove build artifacts"
	@echo "  make realclean    Remove build directories entirely"
	@echo
	@echo "Build directories:"
	@echo "  $(BUILD_HW)/      ARM cross-build (core.elf, core.bin, core.ipod)"
	@echo "  $(BUILD_SIM)/     host build"

hw: $(BUILD_HW)/build.ninja
	ninja -C $(BUILD_HW)

# `make ipod` produces $(BUILD_HW)/core.ipod by running the host Go CLI
# against the flat binary that `make hw` already emits. `go run` here
# avoids requiring a pre-built `core` binary; once the CLI ships in CI
# we'll switch this to the released binary. --force because remaking
# over a previous run's core.ipod is the normal case here, not an
# accident the CLI's overwrite guard should block.
ipod: hw
	cd cli && go run ./cmd/core firmware pack ../$(BUILD_HW)/core.bin --out ../$(BUILD_HW)/core.ipod --force

sim: $(BUILD_SIM)/build.ninja
	ninja -C $(BUILD_SIM)

# Static verification of the hw image that needs the linked ELF:
# crt0/linker layout asserts, plus the header<->doc address
# consistency check (also run standalone in `meson test`). Depends on a
# fresh `make hw`. The MMIO trace tests run under `meson test`.
verify-hw: hw
	sh tests/scripts/check_hw_layout.sh $(BUILD_HW)/core.elf
	sh tests/scripts/check_size.sh $(BUILD_HW)/core.elf
	python3 tests/scripts/check_hw_consistency.py
	python3 tests/scripts/check_name_hash_parity.py

$(BUILD_HW)/build.ninja:
	meson setup $(BUILD_HW) --cross-file cross/arm-none-eabi.txt -Dtarget=hw

$(BUILD_SIM)/build.ninja:
	meson setup $(BUILD_SIM) -Dtarget=sim

reconfigure-hw:
	meson setup --reconfigure $(BUILD_HW) --cross-file cross/arm-none-eabi.txt -Dtarget=hw

reconfigure-sim:
	meson setup --reconfigure $(BUILD_SIM) -Dtarget=sim

clean:
	@if [ -d $(BUILD_HW)  ]; then ninja -C $(BUILD_HW)  -t clean; fi
	@if [ -d $(BUILD_SIM) ]; then ninja -C $(BUILD_SIM) -t clean; fi

realclean:
	rm -rf $(BUILD_HW) $(BUILD_SIM)
