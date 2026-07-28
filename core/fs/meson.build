# Filesystem readers (portable, no hardware access). Grows as needed:
# fat32 (read-only) today; the block source is the caller's callback
# (ata_read_sectors on hw).
#
# m3u.c sits ON TOP of fat32.c rather than beside it: it parses a text file
# the volume already knows how to read, and its only disk dependency is
# fat32_stream_read. It is compiled unconditionally (a few hundred bytes of
# text) so the parser and its host tests can never drift out of the build.
fs_sources = files('fat32.c', 'm3u.c')
