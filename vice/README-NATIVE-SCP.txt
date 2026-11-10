Native SCP implementation: read-only drive and sector-view preview
=================================================

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
* The low-level VICE filesystem-image adapter now recognizes SCP before
  size-based probes, owns/disposes its capture and opens it through MODE_READ
  even when writable attachment was requested. Recognized invalid SCP cannot
  fall through to D64 probing. Native write entry points reject SCP by type.
* A native flux_playback_t scheduler using integer 12.5 ns ticks, with no
  angular normalization. Source timing is preserved independently of the
  16 MHz electronics adapter. Turn/index metadata is not an emulated
  1541 index signal.
* Distinct captured turns, including the first uncued window, overflow carry,
  exact boundary events, an explicit capture-loop seam, motor freeze/resume,
  and deterministic head stepping between tracks with unequal durations.
* Snapshot-ready integer cursor state and validated, atomic reconstruction.
* Sparse seek checkpoints and next-edge indexing bound stream scanning even
  across long overflow-only gaps. No allocation or I/O occurs while advancing
  the cursor. Zero-delay boundary pulses are consumed exactly once.
* Shared 1541 read-side limit/clock helpers extracted from rotation_1541_p64.
  P64 retains its input/weak-pulse handling, write path and circuit ordering.
  The native rotation_flux_t adapter feeds that same filter, counters, RNG,
  sync and byte-ready circuit from exact captured transition timestamps.
* Independent reference-clock phase across catch-up partitions and head
  changes. All intervening transitions reach the filter, including multiple
  pulses between reference clocks. At an exact tie, the circuit clock runs
  before the flux transition, matching P64's ordering. Motor off freezes both
  timelines. A measured edge at the capture endpoint is retained at the loop
  seam; a seam without such an edge does not invent one.
* Explicit unbind removes borrowed capture pointers before owner cleanup.
  Advancing the adapter performs no allocation or I/O and consumes all turns.
* Drive-owned native contexts, CPU-clock/bus-delay accounting, head stepping,
  motor edges, reset and shutdown are connected to normal drive dispatch.
  Native time bypasses synthetic RPM/wobble and P64's skipped-turn catch-up.
  SCP attachment requires true-drive 1541/1541-II with device traps disabled.
  Unsupported attachment is rejected before replacing the current disk.
* Native media sensing reports a present, permanently write-protected disk.
  A real KERNAL SAVE attempt reports DOS 26 (WRITE PROTECT ON). Turning off
  true-drive emulation is rejected while a native image is attached.
* Required native snapshots: DRIVE 2.1 selects mandatory FLUX<unit>:0 1.0;
  other drives keep DRIVE 2.0. Captures serialize independently of SCP/P64,
  with exact cursor/reference-clock and existing circuit/bus-delay state.
  Embedded restoration creates an owned in-memory image, never an SCP file.
  Nonembedded restoration requires the identical capture already attached,
  checked with a canonical SHA-1 identity. It does not reopen a filename or
  read a hardware device. Missing/mismatched captures fail explicitly.

Current scope / limits
----------------------
Native read-only file attachment and 1541 playback are enabled in x64sc.
This is a development preview; the complete first milestone is not finished.
Native SCP autostart, filesystem directory previews and c1541 read/extract
are enabled. Attachment/autostart still requires a 1541/1541-II, true-drive
emulation enabled and device traps disabled. AutostartHandleTrueDriveEmulation
may be enabled or disabled: native loading always uses the live drive circuit.
GTK attach/smart-attach disk filters now include SCP with any letter case.
That frontend change is source-reviewed only: this toolchain has no GTK headers.
SDL's file picker already lists all files and needs no extension filter change.
P64's extracted circuit behavior is checked against pristine upstream code.
Read-only input keeps feeding the read circuit if a write gate is asserted;
physical write-encoder behavior is outside this internal read-side adapter.

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
73 native Windows tests compile the actual production modules with strict
warnings, and inject allocation/read/seek failures. The suite includes 100
random captures compared against an independent timestamp oracle, random
catch-up partitions, 1,000 deterministic parser mutations, full-width integer
phase mapping and allocation accounting. This Windows run is not sanitized.
Tests use generated data only and require no VICE/P64 build or hardware SDK.

18 additional native Windows integration tests compile the actual diskimage,
filesystem lifecycle, format probes and SCP adapter with instrumented platform
services. They verify handle modes, failure cleanup, legacy D64 open policy,
write rejection and malformed-SCP probe ordering. They require a configured
VICE build for platform headers. Example (SDL headers may need --include-dir):
  python tests/native-flux/build-lifecycle.py OUTPUT --configured-build BUILD --cc GCC

23 circuit tests compile actual modified rotation.c and untouched upstream
rotation.c into separate native libraries. They compare 3,388 P64 read/write,
wrap and CPU/bus-delay trace points; native input is checked against individual
upstream reference clocks through 60 randomized, partitioned input scenarios.
Known GCR bytes, sync and byte-ready decode at all four density settings with
three input clock phases. Motor pause, head phase, overflow gaps, endpoint
retention/restoration and ownership are also covered, along with CPU dispatch,
bus delays, motor edges, long catch-up and reset.
21 independent sector-view checks additionally cover checksums, invalid GCR,
missing sectors/tracks, all density zones, extended tracks, capture-boundary
wrapping, multiple revolutions, cache reuse and allocation failure cleanup.
The lifecycle builder also runs these checks. Lower-level total: 135 checks.
  python tests/native-flux/build-electronics.py OUTPUT --configured-build BUILD --cc GCC
The builder extracts the pinned upstream reference from git, or accepts
--reference-rotation PATH. These tests require configured VICE headers but no
emulator UI or hardware SDK. GCC static analysis passes rotation.c and the
modified capture/cursor modules. This Windows run is not sanitizer-enabled.

A native integration link also used real VICE zfile/platform services and
confirmed that its SCP handle rejects fwrite on generated data. D64/G64/P64
true-drive load and utility extraction regressions passed, as did D64/G64
snapshot restoration. The upstream P64 snapshot version mismatch remains.
A subsequent clean native Windows build compiled x64sc and c1541 from a new
source copy and an initially empty build directory. No baseline objects were
reused. GCC 16.2.0/MinGW and SDL2 were used, without WSL. Build-copy scripts and
vice.texi needed LF line endings for upstream generators. The monitor library
and Windows resource target were built explicitly, and reSID-fp's static
archive was made directly from fresh objects to work around a libtool mismatch.
This builds the x64sc/c1541 targets, not every VICE machine or the GTK frontend.

Generated-disk acceptance tests exercise actual KERNAL LOAD on 1541 and
1541-II, DOS write protection, embedded restoration with no source file,
external-capture identity, active-transfer snapshot continuation, rejection by
an old upstream executable, malformed required state and legacy restoration.
Run on Windows with a runtime containing x64sc, ROMs and DLLs:
  python tests/native-flux/test_native_drive.py NEW_OUTPUT --runtime RUNTIME
Optional: --disk-tool C1541 --reference-runtime PRISTINE --legacy-snapshot VSF
The output directory must be new. Fixtures are generated there; user images
are never used by these tests. These checks do not establish compatibility
with every protected disk or a real SuperCard Pro device.

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
Complete interactive directory-selection verification and GTK frontend
testing. Then complete real-capture regression,
performance and a clean Windows build for the first milestone. Hardware
acquisition remains a later adapter feeding the same capture contract.

Manual launch (Windows example)
-------------------------------
  x64sc.exe -default -drive8type 1542 -drive8truedrive +trapdevice8 +autostart-handle-tde -attach8ro -8 "disk.scp"
At the C64 prompt:
  LOAD "*",8,1
  RUN
Native SCP remains read-only regardless of the writable-attachment preference.

Independent DOS sector view
---------------------------
flux_view_t borrows the sealed capture and owns private playback/read-circuit
state and a lazy per-track sector cache. It cannot advance the live drive.
It accepts valid GCR headers/data with matching track/sector and checksums;
the first valid captured copy wins, and later revolutions can recover earlier
bad copies. Read errors leave the caller's sector buffer unchanged. This is
a utility view of ordinary DOS sectors, not a replacement for native playback
of protected tracks. The scan is bounded to 400 ms per captured window plus
one loop for seam-spanning blocks; unobserved tails reset the block parser.
Images with longer windows can play natively even if this view misses sectors.
No write operation is implemented. Snapshot-owned captures also support the
view without needing an open file handle.

Windows generated-image acceptance additionally verifies SCP wildcard
autostart on both supported models with automatic TDE handling on/off, and
directory listing/extraction that matches the source PRG byte-for-byte.
The original/generated SCP hash remains unchanged throughout the tests.

Autostart example (Windows)
--------------------------
  x64sc.exe -default -drive8type 1542 -drive8truedrive +trapdevice8 -attach8ro -autostart "disk.scp"

Clean Windows preview verification
---------------------------------
The clean executables passed the generated-image native acceptance suite,
including directory-index selection/autostart of SUCCESS, wildcard autostart
on both models with automatic TDE handling on/off, exact utility extraction,
true-drive LOAD, DOS 26 write protection and native snapshot checks. Legacy
D64/G64/P64 LOAD/extraction and D64/G64 snapshot checks passed. The previously
identified upstream P64 snapshot version mismatch still returns error 143.

The SDL window booted to BASIC READY, but the desktop helper did not reliably
deliver keyboard input/capture the menu. Interactive directory selection is
therefore unverified. Its production directory-index/autostart backend passed
through the binary monitor. No user images were opened or written this stage.

Reproducing the native build
---------------------------
Use an MSYS2 MinGW64 shell with GCC, SDL2/SDL2_image, zlib, iconv, autotools,
flex/bison and the upstream build prerequisites installed. In a private source
copy, convert *.sh, *.sed, *.awk and doc/vice.texi from CRLF to LF if needed.
Run autogen.sh, create an empty sibling build directory, and configure with:
  ../SOURCE/configure --enable-sdl2ui --disable-arch --without-libcurl --disable-pdf-docs --disable-html-docs
Then, from the build directory:
  make -j4 -C src/lib/linenoise-ng
  make -j4 -C data/common x64sc.res
  make -j4 -C src/lib/libresidfp
If libresidfp.a is missing while its objects exist (the observed toolchain case):
  (cd src/lib/libresidfp; ar crs src/.libs/libresidfp.a src/libresidfp_la-*.o src/resample/libresidfp_la-*.o src/residfp/libresidfp_la-*.o)
Build the executables, keeping the freshly assembled sound archive:
  make -j4 -C src -o ../src/lib/libresidfp/src/.libs/libresidfp.a x64sc.exe
  make -j4 c1541
No objects or archives from a different emulator build are required.

Rejected SCP autostart
----------------------
The complete SCP signature is authoritative during autodetection. If its disk
validation, native attachment or directory selection fails, autostart returns
an error rather than falling through to tape/snapshot/PRG detection. The guard
reads the signature through a read-only zfile handle, so it does not depend on
the filename extension. Full validation remains in the disk-image adapter.
Failure also clears saved autostart settings without injecting virtual-drive
state. Ordinary non-SCP program/disk loading keeps the existing detection path.

Regression checks reproduce the previous malformed-SCP-as-PRG acceptance and
verify rejection of malformed, truncated and unsupported captures plus invalid
directory-index selection. They compare the mounted native capture/cursor
snapshot before/after failure and then load a program from that same disk.
The full generated-disk native acceptance and legacy regression suites pass.
The updated executable recompiles autostart.c and relinks against the earlier
clean Windows build's objects; it is not another empty-directory rebuild.

The updated desktop helper was retried for interactive SDL directory selection.
It still returned another application's image while identifying the window as
VICE, including after window recovery. UI input was stopped. That UI check and
the GTK frontend remain unverified. Generated test disks only were used here;
no original user captures were accessed this stage.
