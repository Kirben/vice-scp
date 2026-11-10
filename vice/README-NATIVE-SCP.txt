Native read-only SCP playback: Windows development preview
=========================================================

Source base: VICE-Team/svn-mirror
Commit: 8e45817287033b36112f6925a89d9f9c953a0cc8
Branch: native-scp-playback
This is a custom development build, not an official VICE release. The first
milestone remains incomplete pending frontend verification and wider coverage.

Using the Windows preview
-------------------------
Extract the whole package and retain its supplied ROMs and DLLs.
Drag an SCP image onto Autostart-SCP-readonly.bat, or run:
  x64sc.exe -default -drive8type 1542 -drive8truedrive +trapdevice8 -attach8ro -autostart "disk.scp"
To attach without autostart, replace -autostart with -8, then use:
  LOAD "*",8,1
  RUN
1541-II uses model number 1542; the original 1541 uses 1541.
Earlier real-capture checks reached program screens for Ghostbusters, KickButt
Side 3, Robots of Dawn and Music Studio. Music Studio was verified with:
  x64sc.exe -default -model c64ntsc -drive8type 1541 -drive8truedrive +trapdevice8 -attach8ro -autostart "disk.scp"
These checks did not verify full gameplay or every protection check.

Configure every drive model before mounting native media. Units 8 through 11,
drive 0, support native SCP on 1541/1541-II. Two-unit tests cover units 8 and 9.
True-drive emulation must be enabled and that unit's device traps disabled.
The default capture-speed policy expands 360 RPM SCP timing by 6/5 for the
nominal 300 RPM 1541. The file and its captured flux are unchanged. To play a
flagged image at its captured clock instead, use -drive8fluxspeed 1 before
attachment (0 restores automatic selection). Detach before changing policy.
AutostartHandleTrueDriveEmulation can be on or off; actual SCP loading always
uses the live emulated drive, not virtual-sector loading or RAM injection.

Protection and settings
-----------------------
SCP is permanently read-only in this implementation. Sector/track write APIs
reject it, its file handle is read-only, and an actual SAVE reports DOS 26.
The attachment read-only preference only affects future attachments when SCP
is mounted; toggling it preserves the capture, cursor and active transfer.
The SDL drive-settings menu includes "SCP media: Read only" for mounted SCP.
Changing the current unit's true-drive setting to off, or enabling its virtual
traps, is rejected while native SCP is mounted. Selecting the current model
is harmless. Actual model changes on ANY unit require all native images to be
detached first, because hardware changes reconfigure the shared IEC bus.

The c1541 utility provides a private DOS-sector view, for example:
  c1541.exe -attach "disk.scp" -list
  c1541.exe -attach "disk.scp" -read "program" "program.prg"
It validates GCR headers/data and checksums, selecting the first valid copy
across captured turns. It owns separate read-circuit/cursor state and cannot
advance a running drive. Read errors leave the destination sector unchanged.
This ordinary-sector view does not replace native playback of protected data.
Its scan is bounded to 400 ms per captured window plus a loop for seam-spanning
blocks. Longer captures can play natively even if the utility misses sectors.

Snapshots
----------
Native drives use DRIVE 2.1 with a required FLUX<unit>:0 module. Version 1.1
stores the selected speed and fractional phase for 360 RPM captures; ordinary
captures retain 1.0. Older 1.0 snapshots still restore at their captured clock.
Other drives retain their legacy format. Older readers reject unknown states.
Embedded snapshots own restored in-memory captures; no SCP file is created or
reopened. Without embedding, the identical capture must already be attached,
verified by its canonical SHA-1 identity. Missing, swapped or inconsistent
required state returns an error. Captures remain read-only after restoration.
Tests cover exact per-unit state, restoration during active loading, source
files unavailable, two mounted captures, and detaching one without losing the
other. Malformed/unsupported SCP and invalid directory selection cannot fall
through to PRG/tape/snapshot autostart after disk rejection.

Native architecture and supported inputs
----------------------------------------
SCP is parsed into a sealed, provider-neutral flux_capture_t with packed
intervals, sparse seek checkpoints and original per-turn durations. A separate
flux_playback_t advances exact integer 12.5 ns timestamps into the shared
1541 read electronics, sampled on its 16 MHz clock. A 360 RPM hint selects
exact 6/5 wall-time expansion, retaining fractional phase across updates,
head changes, motor stops and snapshots. It does not fit each turn to a fixed
duration, erase jitter, or convert to P64. The private DOS sector view uses
the same automatic speed correction. Drive advancement does no allocation
or file I/O.
The image owns its capture; drive cursors are unbound before owner cleanup.
The capture/cursor modules have no USB or hardware SDK dependency. Future
SuperCard Pro acquisition can provide the same data contract; no hardware
access or unused hardware framework is included in this milestone.

Supported: C64, single-sided/side 0, 16-bit captures through track 42.5, including
the historical HEADS=0 encoding and validated short 164/166-entry tables.
Other-media, top-side/two-sided layouts are rejected. Source limits are 1 GiB
and 32 million packed words, plus checkpoints/descriptors and a temporary
per-track buffer. Signature, offsets, sizes, checksum, MODE and footer metadata
are checked. A full SCP signature is authoritative even with another extension.
The parser distinguishes nonmatch, valid and recognized-invalid input.

Captured turns retain their measured timing, including uncued first windows
and overflow carry. Finite captures loop; unrecorded weak-bit variation is not
invented. A real edge at the endpoint appears once at the loop seam; no edge
is synthesized at an empty seam. Motor-off freezes playback. Head changes
preserve turn count and map fractional phase to the target track once; unknown
cross-track alignment cannot be reconstructed. Missing tracks use the most
recent duration, or the nominal acquisition period when initially absent
(200 ms at 300 RPM, about 166.67 ms at 360 RPM before compensation).
The timestamp precision is not a claim of 12.5 ns analog decoder accuracy.
Read-side circuitry keeps receiving captured input if the write gate is set;
physical write-encoder behavior and SCP writing are outside this milestone.

Reproducible validation
-----------------------
Run from the source's vice directory with MinGW GCC on Windows:
  python tests/native-flux/build.py OUTPUT --cc PATH_TO_GCC
  python tests/native-flux/build-lifecycle.py OUTPUT --configured-build BUILD --cc PATH_TO_GCC --include-dir PATH_TO_SDL2_HEADERS
  python tests/native-flux/build-electronics.py OUTPUT --configured-build BUILD --cc PATH_TO_GCC
These run 73 core, 18 filesystem lifecycle, 23 sector-view and 25 circuit checks
(139 total). Circuit tests compare production behavior against the bundled,
hash-verified pristine upstream rotation.c; no Git history is needed. A custom
--reference-rotation path must contain that same pinned file. Core tests include
fault injection, independent timestamp oracles and parser mutations. Circuit
checks compare P64 behavior, clock partitions, all density zones and dispatch.
These Windows tests are not sanitizer-enabled.

For generated-disk emulator tests, supply a runtime with ROMs/DLLs and c1541:
  python tests/native-flux/test_native_drive.py NEW_OUTPUT --runtime RUNTIME
  python tests/native-flux/test_native_drive.py NEW_OUTPUT --runtime RUNTIME --multiunit-only
  python tests/native-flux/test_native_drive.py NEW_OUTPUT --runtime RUNTIME --multirev-only
  python tests/native-flux/test_native_drive.py NEW_OUTPUT --runtime RUNTIME --capture-speed-only
Output directories must be new. Fixtures are generated there, never user disks.
The main suite accepts --reference-runtime PRISTINE and --legacy-snapshot VSF.
It checks actual KERNAL LOAD/SAVE, autostart, snapshot round-trips, rejected
input/settings, model guards, preferences and continued operation after errors.
The two-unit suite uses distinct payloads and clears each expected destination
before LOAD. It checks offline embedded restoration, active unit-9 snapshots,
capture identities, per-unit detachment and shared-bus model protection.
The multi-revolution suite builds continuous interval streams across index
boundaries, including overflow carry, two good turns and three unequal-duration
turns. It checks both 1541 models, exact extraction, capture replacement on the
same unit, and external/embedded multi-turn snapshots with offline sources.
An additional adverse fixture alternates a completely blank turn with a good
turn. Its sector view recovers the program, while native KERNAL LOAD outcomes
are recorded separately in multirev-observations.json, not counted as passes.
The capture-speed suite checks generated 360 RPM media, both drive ROM models,
the captured-clock override, and active/offline snapshot resumption with an
exact nonzero fractional phase. Invalid speed/phase fields are rejected.
For firmware-level diagnostics use --blank-turn-only, optionally selecting a
case with repeated --blank-turn-case NAME. --blank-turn-trace verifies the
1541-II ROM bytes before tracing sync success/timeouts. Trace cursor snapshots
are observations at the deferred monitor stop, not exact checkpoint timestamps.
The diagnostic records KERNAL A/carry, DOS status and exact loaded bytes.
Cases containing "bounded" use a 200 ms total timeline. Compile the optional
tests/native-flux/scp-p64-fixture.c with upstream src/lib/p64/p64.c and configured
headers, then pass --pulse-fixture-tool TOOL --reference-runtime PRISTINE to
compare these cases against upstream P64. The exporter requires exact 16 MHz
positions and a 200 ms period; it performs no timing normalization. Selecting
both NAME and NAME-p64 checks their KERNAL/DOS/payload results agree.

test_real_captures.py NEW_OUTPUT --source INPUT_DIRECTORY --runtime RUNTIME
copies every SCP first and SHA-256 verifies it before any emulator test. Only
those read-only copies are passed to c1541/x64sc, and original/copy hashes are
checked afterwards. Its screenshots require human inspection: "Autostart:
Done" can follow an aborted loader and does not itself prove a successful boot.

Building a fresh native Windows executable
------------------------------------------
Use an MSYS2 MinGW64 shell with GCC, SDL2/SDL2_image, zlib, iconv, autotools,
flex/bison and the usual upstream build prerequisites. No WSL is needed.
In a private source copy, normalize *.sh, *.sed, *.awk and doc/vice.texi to LF
if the checkout uses CRLF; upstream generators need those line endings.
Run ./autogen.sh, then configure from a new sibling build directory:
  ../SOURCE/configure --enable-sdl2ui --disable-arch --without-libcurl --disable-pdf-docs --disable-html-docs --disable-dependency-tracking
An optional configure cache is usable only with the unchanged toolchain and
environment. It does not replace compilation of any emulator objects.
From the build directory:
  make -j4 -C src infocontrib.h
  make -j4 -C src/lib/linenoise-ng
  make -j4 -C data/common x64sc.res
  make -j4 -C src/lib/libresidfp
If the toolchain creates only the sound DLL/import library, make its static
archive directly from those freshly compiled objects:
  (cd src/lib/libresidfp; ar crs src/.libs/libresidfp.a src/libresidfp_la-*.o src/resample/libresidfp_la-*.o src/residfp/libresidfp_la-*.o)
Then build the executables, retaining that archive:
  make -j4 -C src -o ../src/lib/libresidfp/src/.libs/libresidfp.a x64sc.exe
  make -j4 -C src c1541.exe
No objects/archives from another emulator build are required. These targets
build x64sc/c1541, not every VICE machine or the GTK frontend.

Known limits and remaining work
-------------------------------
* Interactive SDL directory selection and visual inspection of the media
  indicator remain unverified: the desktop helper returned another app's
  image for the VICE window, so automated UI input was stopped.
* GTK attach/smart-attach SCP filters are implemented but not built/tested.
* Capture-speed compensation trusts the 360 RPM header hint. It cannot infer
  a mislabelled acquisition clock or reconstruct uncaptured angular alignment.
  Use captured-clock mode for a flagged file that already has nominal timings.
* Wider gameplay, later loading stages, disk swaps and multi-revolution real
  captures still need compatibility coverage. Initial screens are not proof
  of full-disk readability or support for every copy-protection scheme.
  The nine-image 360 RPM run reached program screens for Cauldron, Ghostbusters,
  Kick Butt, Murder, Robots of Dawn and The Untouchables with PAL/1541-II.
  Rambo II reached its instructions on a longer PAL/1541-II run. Jaws reached
  its side-2 prompt using -model c64ntsc -drive8type 1541; its other side is
  needed to continue. Ghetto Blaster has not reached a playable screen.
* Whole blank turns can cause a genuine DOS 21 no-sync error despite successful
  sector-view salvage. The 1541-II firmware limits each sync search to about
  20 ms. Native/pristine P64 comparisons agree for identical 200 ms timelines:
  5/10 ms blank gaps load; a 20 ms gap reports DOS 21. The full 200 ms blank
  window and real-pulse resumption also match the pristine circuit over two
  loops. Playback keeps blank intervals; it does not select a good turn to
  bypass firmware errors. These diagnostics do not prove every adverse image
  must fail or exhaustively validate real hardware behavior.
* A hot drive-model-change load stall was reproduced in pristine upstream
  with generated G64 images. The native guard prevents that reconfiguration
  while SCP is mounted; it does not change legacy-only model-switch behavior.
* The upstream P64 snapshot writer/reader version mismatch still returns 143.
  D64/G64 snapshots and all three legacy LOAD/extraction regressions pass.
* Native SCP writes and physical SuperCard Pro access are separate milestones.

Keep original preservation images untouched and use working copies for manual
testing. Automated checks in the current build stage use generated disks only.
