Native SCP disk images
======================

VICE uses SCP flux transitions directly for native disk playback. Enable
true drive emulation and disable device traps for the attached unit. For
example, to autostart a read-only image with a 1541-II:

  x64sc -drive8type 1542 -drive8truedrive +trapdevice8 -attach8ro -autostart disk.scp

Use -8 disk.scp to attach without autostart. Replace 8 in the options with
the desired unit number. Configure the drive before attaching the image.

Drive and capture layouts
-------------------------

The 1540, 1541, 1541-II, 1551, 1570 and 2031 use single-sided GCR.
The 1571 and 1571CR also support double-sided GCR with interleaved
full-track entries. The 2040, 3040, 4040, 8050 and 8250 support both
mechanisms. The 8050, 8250 and SFD-1001 use their corresponding wide GCR
geometries. The 1581 and CMD FD drives support compatible MFM captures.

A compatible top-side-only capture can use its recorded head directly.
On a 1541 or 1541-II, set -drive8scpflip before attachment to play a
compatible top-side capture as a flipped disk. Writes use the original
physical slots and sample grid.

The automatic playback clock compensates for 360 RPM acquisition
when appropriate. Set -drive8fluxspeed 1 before attachment to use the
recorded clock. Set -drive8fluxalign to an odd half-track number to
read its adjacent lower full-track capture. For example, 37 selects
track 18.5. A mount using this alignment override is write protected.

Writing and formatting
----------------------

Use a writable working copy as an uncompressed SCP or a gzip-compressed SCP.
Single-revolution images require writable MODE. For eligible multi-revolution images, set
-scpmultirevwrite before attaching. Explicit read-only mounts take
precedence. c1541 provides a read-only sector view of compatible SCPs.

Create an SCP with Create and Attach Disk Image. Issue the emulated
drive's format command with a disk ID, for example N0:DISK,42 on a
1541. On the 1571 and 1571CR, send U0>M1 before formatting both sides.
On a dual IEEE drive, select the mechanism in the format command,
for example N1:DISK,42.

Empty SCPs use the attached drive's geometry. The default MFM densities
are DD for the 1581, HD for the FD-2000 and ED for the FD-4000. To select
a supported alternate density, set -drive8scpblankdensity before
attaching an empty image (0 for default, 1 for DD, 2 for HD, 3 for ED).
Use the corresponding CMD DOS format option, such as N:DISK,42,DDN.

The writer retains the sample grid, captured revolution count, declared
track range, and original revolution payloads on untouched tracks.
Supported metadata includes SCP 1.6 footers, the Greaseweazle 2.4 footer
layout and a standard WRSP extension. Unknown extensions and resolutions
above the controller limit are write protected. Interleaved 1571 images
can store writes at full-track positions. Unindexed partial writes
require a measured period or consistent alignment across the affected
windows. Full formatting intentionally replaces flux in the formatted
geometry.

Detach the disk or exit normally to publish changes. If publication
cannot proceed safely, VICE retains the backing file and reports the
recovery image path when one can be created. Gzip images are recompressed
before replacement. A recovery image from a gzip mount is a plain SCP.

Snapshots
---------

Embedded snapshots restore the captured media in memory. An embedded
writable disk has no backing SCP file. Include disks in a subsequent
snapshot to retain further changes. For an external snapshot, attach
matching media with matching write protection before restoring.

Developer validation
--------------------

Run from the source's vice directory:

  python tests/native-flux/build.py OUTPUT --cc PATH_TO_GCC
  python tests/native-flux/build-lifecycle.py OUTPUT --configured-build BUILD --cc PATH_TO_GCC --include-dir PATH_TO_SDL2_HEADERS
  python tests/native-flux/build-electronics.py OUTPUT --configured-build BUILD --cc PATH_TO_GCC
  python tests/native-flux/test_native_drive.py NEW_OUTPUT --runtime RUNTIME

The standalone suite includes the writer round-trip tests. The runtime
suite uses an emulator package with ROMs and c1541 and generates its own
disk images. See each script's --help for additional test options.
