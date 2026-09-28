# LEAP source used by NavigaMer

These files are the minimal AVX2 LEAP distance implementation imported from the
working tree at `experiments/leap-quick-R6874j/leap`. Its base revision is
`6169540a74eaaedb3f95c04b661e0bc1a8b65c51` from
<https://github.com/Naomigh/leap>.

The imported `SIMD_ED.cc` and `shift.c` include the local correctness fixes for
state reset, AVX2 cross-128-bit shifts, global-alignment completion, the middle
lane offset, and safe pruning. At import time their SHA-256 values were:

- `SIMD_ED.cc`: `4a1f24aa73c866560cb0d68a3a988ce24f2c5d8e64e5ae3e877153ee4deb3d9d`
- `shift.c`: `1507d7fa88040fdc613b507197ea1b71c4d1504fd0f755c2d16b9790cba6f291`

NavigaMer builds only the AVX2 backend and disables LEAP's SHD filter at runtime.

The match-run scan now uses a signed remaining-length loop condition. The
original `(remaining - 1) / sizeof(...)` converted negative values to unsigned
when a lane reached the sequence end and could read beyond the mask array.
This is exercised by the distance oracle tests under ASan/UBSan.
Cleanup also frees `ED_info`; reinitialization calls the shared cleanup routine
instead of explicitly ending the object's lifetime with a destructor call.
