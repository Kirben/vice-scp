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
compatible top-side capture as a flipped disk. SCP images are mounted read-only.

The automatic playback clock compensates for 360 RPM acquisition
when appropriate. Set -drive8fluxspeed 1 before attachment to use the
recorded clock. Set -drive8fluxalign to an odd half-track number to
read its adjacent lower full-track capture. For example, 37 selects
track 18.5. A mount using this alignment override is write protected.

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
