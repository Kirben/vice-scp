/* Required provider-neutral flux snapshot state. GPL-2.0-or-later. */
#ifndef VICE_DRIVE_FLUX_SNAPSHOT_H
#define VICE_DRIVE_FLUX_SNAPSHOT_H
struct snapshot_s;
struct drive_s;
int drive_flux_snapshot_validate(const struct drive_s *drive, unsigned int halftrack);
int drive_flux_snapshot_write(struct snapshot_s *s, struct drive_s *drive,
                                unsigned int unit, unsigned int number, int embed);
int drive_flux_snapshot_read(struct snapshot_s *s, struct drive_s *drive,
                               unsigned int unit, unsigned int number, unsigned int halftrack);
#endif
