// Package firmware deals with the iPod firmware-partition image format
// (the wire format the boot ROM expects, not our higher-level packaging).
//
// See core/docs/hw/08-boot-dock.md for the full spec.
package firmware

// ModelNum is the additive-checksum seed for each iPod model. The
// firmware-partition checksum is `sum(image_bytes) + ModelNum`,
// 32-bit wrapping. The boot ROM compares this against the value in
// the directory entry; mismatch = it refuses to load.
type ModelNum uint32

// Known model seeds. The set we actually care about is just iPodVideo.
//
// PROVENANCE AND RISK.
//
// ModelIPodVideo = 0x05 was originally asserted here on the strength of
// this project's own doc citing Rockbox line numbers, and
// checksum_test.go tested 0x05 against 0x05 — a self-referential
// assertion that would pass just as happily if the constant were wrong.
// That matters because the shipping path is ipodloader2 chainloading,
// which never exercises the partition checksum: a wrong seed would stay
// invisible until someone implemented `core install`, and would then
// produce images the boot ROM refuses, on hardware, at the least
// convenient moment.
//
// The seed has since been corroborated externally. Two independently
// produced .ipod files with the "ipvd" model name were checked, and in
// both the stored big-endian checksum minus the byte sum of the payload
// is exactly 5:
//
//	rockbox.ipod (Rockbox build, 777976-byte payload)
//	    stored 0x04E076C2, sum 0x04E076BD, difference 5
//	core.ipod (this project, 225388-byte payload)
//	    stored 0x0143A662, sum 0x0143A65D, difference 5
//
// The Rockbox artifact is the meaningful one: it was produced by a
// toolchain that has nothing to do with this code, so it is a genuine
// third-party vector rather than a restatement of our own assumption.
// TestChecksumGoldenVector re-derives the seed from a real file when
// one is provided (see checksum_test.go); no third-party bytes are
// vendored into this repo.
//
// ModelIPodNano = 0x04 has NOT been corroborated this way — no Nano
// .ipod was available. It is unused (we don't target the Nano) and
// should be treated as unverified.
const (
	ModelIPodVideo ModelNum = 0x05
	ModelIPodNano  ModelNum = 0x04 // for completeness; unverified, we don't target it
)

// Checksum computes the additive 32-bit checksum used by the iPod
// firmware-partition image format. Treats `data` as a sequence of
// unsigned bytes and accumulates into a 32-bit value, starting from
// the model seed.
//
// Performance: on Go 1.22 with bounds-check elision this is ~1 GB/s
// on a typical x86-64. We don't optimize further because the checksum
// runs at install time on hosts, not in the firmware hot path.
func Checksum(model ModelNum, data []byte) uint32 {
	c := uint32(model)
	for _, b := range data {
		c += uint32(b)
	}
	return c
}
