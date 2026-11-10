Native SCP implementation: initial core
======================================

Branch: native-scp-playback
Base: 8e45817287033b36112f6925a89d9f9c953a0cc8

Implemented
-----------
* A sealed, provider-neutral flux_capture_t storing host-endian packed
  intervals and exact per-turn durations. It has no P64, USB or filesystem
  dependency. Capture ownership stays with the caller; a playback cursor
  borrows it and must be destroyed/unbound before the capture is released.
* A read-only scp_file_read(FILE *, ...) adapter with checked signature,
  offsets, lengths, checksums, MODE constraints, footer strings and legacy
  164/166-entry tables. It publishes nothing on an error and never writes.
* A native flux_playback_t scheduler using integer 12.5 ns ticks, with no
  angular normalization. Source timing is preserved independently of the
  future 16 MHz electronics adapter. Turn/index metadata is not an emulated
  1541 index signal.
* Distinct captured turns, including the first uncued window, overflow carry,
  exact boundary events, an explicit capture-loop seam, motor freeze/resume,
  and deterministic head stepping between tracks with unequal durations.
* Snapshot-ready integer cursor state and validated, atomic reconstruction.
  No VICE snapshot module has been added yet.
* Sparse seek checkpoints and next-edge indexing bound stream scanning even
  across long overflow-only gaps. No allocation or I/O occurs while advancing
  the cursor. Zero-delay boundary pulses are consumed exactly once.

Current scope / limits
----------------------
These are foundation modules, NOT an enabled VICE SCP backend. File probing,
attachment, vdrive/autostart, electronics coupling and full snapshots remain
to be implemented. Building this tree does not yet make x64sc accept SCP.
Existing image playback paths are unchanged.

The parser accepts C64 single-sided 16-bit captures through track 42.5;
generic/other media and top-side/two-sided layouts are rejected. Source files
are limited to 1 GiB and retained packed intervals to 32 million words (64 MB,
plus checkpoints/descriptors and a temporary per-track input buffer). Images
marked writable are still read-only input; there is no SCP writer.

Each track's captured turn times are independent observations. Head changes
preserve turn count and map fractional angular phase into the destination
turn once. They cannot recover unknown cross-track alignment. Absent tracks
keep the most recent turn duration; binding initially to an absent track uses
an explicit 200 ms fallback. Finite captures loop; missing physical weak-bit
variation is not invented.

The source adapter cannot report a full-signature match until all three SCP
magic bytes are available; a shorter prefix is a nonmatch. A recognized but
malformed SCP returns an error. Scans/cursors must use validated API state;
the borrowing capture must outlive them.

Validation
----------
71 native Windows tests compile the actual production modules with strict
warnings, and inject allocation/read/seek failures. The suite includes 100
random captures compared against an independent timestamp oracle, random
catch-up partitions, 1,000 deterministic parser mutations, full-width integer
phase mapping and allocation accounting. This Windows run is not sanitized.
Tests use generated data only and require no VICE/P64 build or hardware SDK.

Run from this directory:
  python tests/native-flux/build.py OUTPUT --cc PATH_TO_MINGW_GCC
On Linux with GCC/Clang:
  python3 tests/native-flux/build.py OUTPUT --sanitize

scp_file_read returns FLUX_OK (0), SCP_FILE_NOT_MATCHED (1), or a negative
flux_result_t. Callers must compare explicitly, not use a truthy probe check.
On failure, the output capture is NULL and the caller retains FILE ownership.
The capture builder copies its input arrays and rejects mutation after sealing.

Next integration steps
----------------------
Complete the image adapter ownership/read-only attachment path; extract and
regression-test the existing 1541 electronics helpers before adding a native
event source. Then implement independent vdrive decoding, autostart, UI and a
required versioned native snapshot module. Hardware acquisition remains a
later adapter feeding the same capture contract.
