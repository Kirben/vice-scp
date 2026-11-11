/*
 * attach.c - File system attach management.
 *
 * Written by
 *  Andreas Boose <viceteam@t-online.de>
 *
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 *
 */

/* #define DEBUG_ATTACH */

#include "vice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "archdep.h"
#include "attach.h"
#include "cmdline.h"
#include "diskimage.h"
#include "diskimage/fsimage.h"
#include "diskimage/fsimage-scp.h"
#include "drive/rotation-flux.h"
#include "drive/rotation.h"
#include "drive/iec/wd1770.h"
#include "drive/iec/pc8477.h"
#include "driveimage.h"
#include "drive.h"
#include "drive-check.h"
#include "fsdevice.h"
#include "fsimage.h"
#include "fliplist.h"
#include "lib.h"
#include "log.h"
#include "machine-bus.h"
#include "machine-drive.h"
#include "network.h"
#include "resources.h"
#include "serial.h"
#include "types.h"
#include "uiapi.h"
#include "vdrive-bam.h"
#include "vdrive-iec.h"
#include "vdrive.h"
#include "vice-event.h"
#include "p64.h"

#ifdef DEBUG_ATTACH
#define DBG(x) log_printf  x
#else
#define DBG(x)
#endif

typedef struct {
    serial_t *serial;
    vdrive_t *vdrive;
} file_system_t;

#define NUM_DISK_UNITS          4
#define NUM_DRIVES              2

static file_system_t file_system[NUM_DISK_UNITS];

static log_t attach_log = LOG_DEFAULT;

static int scp_multirev_write;
static int attach_device_readonly_enabled[NUM_DISK_UNITS][NUM_DRIVES];
static int file_system_device_enabled[NUM_DISK_UNITS] = { -1, -1, -1, -1 };

static int set_attach_device_readonly(int val, void *param);
static int set_file_system_device(int val, void *param);

static int vdrive_detach_disk_image_and_free(vdrive_t *vdrive,
                                              unsigned int unit,
                                              unsigned int drive);
static int detach_disk_image_and_free(disk_image_t *image, vdrive_t *vdrive,
                                       unsigned int unit, unsigned int drive);
static void detach_disk_image(disk_image_t *image, vdrive_t *vdrive,
                              unsigned int unit, unsigned int drive);
static int attach_disk_image(disk_image_t *oldimage, vdrive_t *vdrive,
                             const char *filename, unsigned int unit,
                             unsigned int drive,
                             int devicetype);

#define UNIT_AND_DRIVE(unit, drive)     ((unit << 8) | drive)
#define GET_UNIT(du)                    ((du >> 8) & 0xFF)
#define GET_DRIVE(du)                   (du & 0xFF)

static int set_scp_multirev_write(int value, void *param)
{
    scp_multirev_write = !!value;
    return 0;
}

static const resource_int_t resources_int[] = {
    { "SCPMultiRevWrite", 0, RES_EVENT_SAME, NULL,
      &scp_multirev_write, set_scp_multirev_write, NULL },
    { "AttachDevice8d0Readonly", 0, RES_EVENT_SAME, NULL,
      &attach_device_readonly_enabled[0][0],
      set_attach_device_readonly, (void *)UNIT_AND_DRIVE(8,0) },
    { "AttachDevice9d0Readonly", 0, RES_EVENT_SAME, NULL,
      &attach_device_readonly_enabled[1][0],
      set_attach_device_readonly, (void *)UNIT_AND_DRIVE(9,0) },
    { "AttachDevice10d0Readonly", 0, RES_EVENT_SAME, NULL,
      &attach_device_readonly_enabled[2][0],
      set_attach_device_readonly, (void *)UNIT_AND_DRIVE(10,0) },
    { "AttachDevice11d0Readonly", 0, RES_EVENT_SAME, NULL,
      &attach_device_readonly_enabled[3][0],
      set_attach_device_readonly, (void *)UNIT_AND_DRIVE(11,0) },
    { "AttachDevice8d1Readonly", 0, RES_EVENT_SAME, NULL,
      &attach_device_readonly_enabled[0][1],
      set_attach_device_readonly, (void *)UNIT_AND_DRIVE(8,1) },
    { "AttachDevice9d1Readonly", 0, RES_EVENT_SAME, NULL,
      &attach_device_readonly_enabled[1][1],
      set_attach_device_readonly, (void *)UNIT_AND_DRIVE(9,1) },
    { "AttachDevice10d1Readonly", 0, RES_EVENT_SAME, NULL,
      &attach_device_readonly_enabled[2][1],
      set_attach_device_readonly, (void *)UNIT_AND_DRIVE(10,1) },
    { "AttachDevice11d1Readonly", 0, RES_EVENT_SAME, NULL,
      &attach_device_readonly_enabled[3][1],
      set_attach_device_readonly, (void *)UNIT_AND_DRIVE(11,1) },
    { "FileSystemDevice8", ATTACH_DEVICE_FS,
      RES_EVENT_STRICT, (resource_value_t)ATTACH_DEVICE_FS,
      &file_system_device_enabled[0],
      set_file_system_device, (void *)8 },
    { "FileSystemDevice9", ATTACH_DEVICE_FS,
      RES_EVENT_STRICT, (resource_value_t)ATTACH_DEVICE_FS,
      &file_system_device_enabled[1],
      set_file_system_device, (void *)9 },
    { "FileSystemDevice10", ATTACH_DEVICE_FS,
      RES_EVENT_STRICT, (resource_value_t)ATTACH_DEVICE_FS,
      &file_system_device_enabled[2],
      set_file_system_device, (void *)10 },
    { "FileSystemDevice11", ATTACH_DEVICE_FS,
      RES_EVENT_STRICT, (resource_value_t)ATTACH_DEVICE_FS,
      &file_system_device_enabled[3],
      set_file_system_device, (void *)11 },
    RESOURCE_INT_LIST_END
};

int file_system_resources_init(void)
{
    return resources_register_int(resources_int);
}

/* ------------------------------------------------------------------------- */

static const cmdline_option_t cmdline_options[] =
{
    { "-scpmultirevwrite", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "SCPMultiRevWrite", (resource_value_t)1,
      NULL, "Allow saving to compatible multi-revolution SCP images" },
    { "+scpmultirevwrite", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "SCPMultiRevWrite", (resource_value_t)0,
      NULL, "Protect multi-revolution SCP images from writes" },
    { "-devicebackend8", SET_RESOURCE, CMDLINE_ATTRIB_NEED_ARGS,
      NULL, NULL, "FileSystemDevice8", (void *)ATTACH_DEVICE_FS,
      "<Type>", "Set device type for device #8 (0: None, 1: Filesystem, 2: OpenCBM)" },
    { "-devicebackend9", SET_RESOURCE, CMDLINE_ATTRIB_NEED_ARGS,
      NULL, NULL, "FileSystemDevice9", (void *)ATTACH_DEVICE_FS,
      "<Type>", "Set device type for device #9 (0: None, 1: Filesystem, 2: OpenCBM)" },
    { "-devicebackend10", SET_RESOURCE, CMDLINE_ATTRIB_NEED_ARGS,
      NULL, NULL, "FileSystemDevice10", (void *)ATTACH_DEVICE_FS,
      "<Type>", "Set device type for device #10 (0: None, 1: Filesystem, 2: OpenCBM)" },
    { "-devicebackend11", SET_RESOURCE, CMDLINE_ATTRIB_NEED_ARGS,
      NULL, NULL, "FileSystemDevice11", (void *)ATTACH_DEVICE_FS,
      "<Type>", "Set device type for device #11 (0: None, 1: Filesystem, 2: OpenCBM)" },
    { "-attach8ro", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AttachDevice8d0Readonly", (resource_value_t)1,
      NULL, "Attach disk image for drive #8:0 read only" },
    { "-attach8rw", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AttachDevice8d0Readonly", (resource_value_t)0,
      NULL, "Attach disk image for drive #8:0 read write (if possible)" },
    { "-attach9ro", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AttachDevice9d0Readonly", (resource_value_t)1,
      NULL, "Attach disk image for drive #9:0 read only" },
    { "-attach9rw", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AttachDevice9d0Readonly", (resource_value_t)0,
      NULL, "Attach disk image for drive #9:0 read write (if possible)" },
    { "-attach10ro", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AttachDevice10d0Readonly", (resource_value_t)1,
      NULL, "Attach disk image for drive #10:0 read only" },
    { "-attach10rw", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AttachDevice10d0Readonly", (resource_value_t)0,
      NULL, "Attach disk image for drive #10:0 read write (if possible)" },
    { "-attach11ro", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AttachDevice11d0Readonly", (resource_value_t)1,
      NULL, "Attach disk image for drive #11:0 read only" },
    { "-attach11rw", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AttachDevice11d0Readonly", (resource_value_t)0,
      NULL, "Attach disk image for drive #11:0 read write (if possible)" },
    { "-attach8d1ro", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AttachDevice8d1Readonly", (resource_value_t)1,
      NULL, "Attach disk image for drive #8:1 read only" },
    { "-attach8d1rw", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AttachDevice8d1Readonly", (resource_value_t)0,
      NULL, "Attach disk image for drive #8:1 read write (if possible)" },
    { "-attach9d1ro", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AttachDevice9d1Readonly", (resource_value_t)1,
      NULL, "Attach disk image for drive #9:1 read only" },
    { "-attach9d1rw", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AttachDevice9d1Readonly", (resource_value_t)0,
      NULL, "Attach disk image for drive #9:1 read write (if possible)" },
    { "-attach10d1ro", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AttachDevice10d1Readonly", (resource_value_t)1,
      NULL, "Attach disk image for drive #10:1 read only" },
    { "-attach10d1rw", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AttachDevice10d1Readonly", (resource_value_t)0,
      NULL, "Attach disk image for drive #10:1 read write (if possible)" },
    { "-attach11d1ro", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AttachDevice11d1Readonly", (resource_value_t)1,
      NULL, "Attach disk image for drive #11:1 read only" },
    { "-attach11d1rw", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "AttachDevice11d1Readonly", (resource_value_t)0,
      NULL, "Attach disk image for drive #11:1 read write (if possible)" },
    CMDLINE_LIST_END
};

int file_system_cmdline_options_init(void)
{
    return cmdline_register_options(cmdline_options);
}

/* ------------------------------------------------------------------------- */

/*
 * Setting the serial hooks is a per-unit thing, it does not make
 * sense (or isn't even possible) to differ between drive 0 and 1.
 *
 * NOTE: it requires that the corresponding vdrive(s) are not NULL.
 *
 * It may seem that int fs is a boolean, but in one location it is called
 * with the value from file_system_device_enabled[i], so in principle
 * it can receive all values ATTACH_DEVICE_NONE, _FS, _REAL and _VIRT. FIXME!
 */
static int file_system_set_serial_hooks(unsigned int unit, int fs)
{
    DBG(("file_system_set_serial_hooks dev %u: %s", unit, (fs == ATTACH_DEVICE_NONE) ? "vdrive" : "fsdevice"));

    if (fs == ATTACH_DEVICE_NONE) {
        if (vdrive_iec_attach(unit, "CBM Disk Drive")) {
            log_error(attach_log,
                      "Could not initialize vdrive emulation for device #%u.",
                      unit);
            return -1;
        }

        return 0;
    } else {
        unsigned int drive = 0;
        int rc = 0;

        for (drive = 0; drive < NUM_DRIVES; drive++) {
            if (fsdevice_attach(unit, drive, "FS Drive")) {
                log_error(attach_log,
                          "Could not initialize FS drive for device #%u.",
                          unit);
                rc = -1;
            }
        }

        return rc;
    }
}

void file_system_init(void)
{
    unsigned int i;

    attach_log = log_open("Attach");

    for (i = 0; i < 8; i++) {
        serial_device_type_set(SERIAL_DEVICE_VIRT, i);
    }

    for (i = 0; i < NUM_DISK_UNITS; i++) {
        file_system[i].serial = serial_device_get(i + 8);
        file_system[i].vdrive = lib_calloc(1, sizeof(vdrive_t));

        switch (file_system_device_enabled[i]) {
            case ATTACH_DEVICE_NONE:
                vdrive_device_setup(file_system[i].vdrive, i + 8);
                serial_device_type_set(SERIAL_DEVICE_NONE, i + 8);
                break;
            case ATTACH_DEVICE_FS:
                vdrive_device_setup(file_system[i].vdrive, i + 8);
                serial_device_type_set(SERIAL_DEVICE_FS, i + 8);
                break;
            case ATTACH_DEVICE_REAL:
                vdrive_device_setup(file_system[i].vdrive, i + 8);
                serial_device_type_set(SERIAL_DEVICE_REAL, i + 8);
                break;
            case ATTACH_DEVICE_VIRT:
                vdrive_device_setup(file_system[i].vdrive, i + 8);
                serial_device_type_set(SERIAL_DEVICE_VIRT, i + 8);
                break;
        }
        if (file_system_device_enabled[i] == ATTACH_DEVICE_VIRT) {
            file_system_set_serial_hooks(i + 8, ATTACH_DEVICE_NONE);
        } else {
            file_system_set_serial_hooks(i + 8, file_system_device_enabled[i]);
        }
    }
}

void file_system_shutdown(void)
{
    unsigned int i;

    for (i = 0; i < NUM_DISK_UNITS; i++) {
        vdrive_device_shutdown(file_system[i].vdrive);
        lib_free(file_system[i].vdrive);
        machine_bus_device_detach(i + 8); /* free memory allocated by file_system_set_serial_hooks() */
    }
}

struct vdrive_s *file_system_get_vdrive(unsigned int unit)
{
    if (unit < 8 || unit >= 8 + NUM_DISK_UNITS) {
        log_error(attach_log, "Wrong unit %u for vdrive", unit);
        return NULL;
    }

    return file_system[unit - 8].vdrive;
}

struct disk_image_s *file_system_get_image(unsigned int unit, unsigned int drive)
{
    return vdrive_get_image(file_system_get_vdrive(unit), drive);
}

const char *file_system_get_disk_name(unsigned int unit, unsigned int drive)
{
    vdrive_t *vdrive;
    disk_image_t *image;

    vdrive = file_system_get_vdrive(unit);
    image = vdrive_get_image(vdrive, drive);

    if (image == NULL) {
        return NULL;
    }
    if (image->device != DISK_IMAGE_DEVICE_FS) {
        return NULL;
    }

    return disk_image_fsimage_name_get(image);
}

int file_system_bam_get_disk_id(unsigned int unit, unsigned int drive, uint8_t *id)
{
    return vdrive_bam_get_disk_id(unit, drive, id);
}

int file_system_bam_set_disk_id(unsigned int unit, unsigned int drive, uint8_t *id)
{
    return vdrive_bam_set_disk_id(unit, drive, id);
}

/* ------------------------------------------------------------------------- */

static int set_attach_device_readonly(int value, void *param)
{
    unsigned int unit = GET_UNIT(vice_ptr_to_uint(param));
    unsigned int drive = GET_DRIVE(vice_ptr_to_uint(param));
    const char *old_filename;
    char *new_filename;
    int rc;
    int val = value ? 1 : 0;
    disk_image_t *image;

    /* Do nothing if resource is unchanged. */
    if (attach_device_readonly_enabled[unit - 8][drive] == val) {
        return 0;
    }

    image = file_system_get_image(unit, drive);
    if (image != NULL && image->type == DISK_IMAGE_TYPE_SCP) {
        /* Keep a mounted SCP's protection fixed until it is detached. */
        if (!image->read_only) {
            log_error(attach_log, "Detach the writable SCP image before changing write protection.");
            return -1;
        }
        attach_device_readonly_enabled[unit - 8][drive] = val;
        return 0;
    }

    old_filename = file_system_get_disk_name(unit, drive);

    /* If no disk is attached, just changed the resource.  */
    if (old_filename == NULL) {
        attach_device_readonly_enabled[unit - 8][drive] = val;
        return 0;
    }

    /* Old filename will go away after the image is detached.  */
    new_filename = lib_strdup(old_filename);

    file_system_detach_disk(unit, drive);
    attach_device_readonly_enabled[unit - 8][drive] = val;

    rc = file_system_attach_disk(unit, drive, new_filename);

    lib_free(new_filename);

    return rc;
}

/* ------------------------------------------------------------------------- */

#if 0
static int vdrive_device_setup_if_no_image(vdrive_t *vdrive, unsigned int unit, unsigned int drive)
{
    if (vdrive != NULL && vdrive_get_image(vdrive, drive) == NULL) {
        return vdrive_device_setup(vdrive, unit /*, drive */); /* FIXME: drive number */
    }

    return 0;
}
#endif

/* ------------------------------------------------------------------------- */

static int set_file_system_device(int val, void *param)
{
    vdrive_t *vdrive;
    unsigned int unit = vice_ptr_to_uint(param);
    unsigned int drive;
    unsigned int idx;
    int old_device_enabled, new_device_enabled;

    if ((unit < 8) || (unit >= 8 + NUM_DISK_UNITS)) {
        DBG(("set_file_system_device invalid dev #%u", unit));
        return -1;
    }
    switch (val) {
        case ATTACH_DEVICE_NONE:    /* fall through */
        case ATTACH_DEVICE_FS:      /* fall through */
        case ATTACH_DEVICE_REAL:    /* fall through */
        case ATTACH_DEVICE_VIRT:
            break;
        default:
            return -1;
    }

    idx = unit - 8;
    old_device_enabled = file_system_device_enabled[idx];
    new_device_enabled = val;

    DBG(("set_file_system_device dev #%u old dev:%d new dev:%d", unit, old_device_enabled, new_device_enabled));

    if (old_device_enabled == new_device_enabled) {
        return 0;
    }

    file_system_device_enabled[idx] = new_device_enabled;

    vdrive = file_system_get_vdrive(unit);

    if (vdrive == NULL) {
        /* file_system_set_serial_hooks() requires non-NULL... */
        DBG(("set_file_system_device: Too early in initialization; unit %u: vdrive is NULL (file_system_init not called?)", unit));
        return 0;
    }

    if (old_device_enabled == ATTACH_DEVICE_REAL) {
        DBG(("set_file_system_device: old == ATTACH_DEVICE_REAL, serial_realdevice_disable()"));
        serial_realdevice_disable();
    }

    if (new_device_enabled == ATTACH_DEVICE_REAL) {
        DBG(("set_file_system_device: new == ATTACH_DEVICE_REAL, serial_realdevice_enable()"));
        if (serial_realdevice_enable() < 0) {
            log_warning(attach_log, "Falling back to fs device.");
            return set_file_system_device(ATTACH_DEVICE_FS, param);
        }
    }

    /*
     * FIXME: Note about ATTACH_DEVICE_FS and ATTACH_DEVICE_VIRT:
     * Attaching a disk image also uses _FS even though you would expect _VIRT.
     * The value _VIRT seems to be unused in practice.
     * One would expect _FS for the fsdevice, and _VIRT for vdrive images.
     */
    switch (new_device_enabled) {
        case ATTACH_DEVICE_NONE:
            DBG(("set_file_system_device: new == ATTACH_DEVICE_NONE"));
#if 0
            for (drive = 0; drive < NUM_DRIVES; drive++) {
                vdrive_device_setup_if_no_image(vdrive, unit, drive);
            }
#endif
            serial_device_type_set(SERIAL_DEVICE_NONE, unit);
            file_system_set_serial_hooks(unit, ATTACH_DEVICE_NONE);
            break;
        case ATTACH_DEVICE_VIRT:
            DBG(("set_file_system_device: new == ATTACH_DEVICE_VIRT"));
#if 0
            for (drive = 0; drive < NUM_DRIVES; drive++) {
                vdrive_device_setup_if_no_image(vdrive, unit, drive);
            }
#endif
            serial_device_type_set(SERIAL_DEVICE_VIRT, unit);
            file_system_set_serial_hooks(unit, ATTACH_DEVICE_NONE);
            break;
        case ATTACH_DEVICE_FS:
            DBG(("set_file_system_device: new == ATTACH_DEVICE_FS"));
            for (drive = 0; drive < NUM_DRIVES; drive++) {
                if (vdrive_detach_disk_image_and_free(vdrive, unit, drive) < 0) {
                    return -1;
                }
                ui_display_drive_current_image(idx, drive, "");
#if 0
                vdrive_device_setup_if_no_image(vdrive, unit, drive);
#endif
            }
            serial_device_type_set(SERIAL_DEVICE_FS, unit);
            file_system_set_serial_hooks(unit, ATTACH_DEVICE_FS);
            break;
#ifdef HAVE_REALDEVICE
        case ATTACH_DEVICE_REAL:
            DBG(("set_file_system_device: new == ATTACH_DEVICE_REAL"));
            for (drive = 0; drive < NUM_DRIVES; drive++) {
                if (vdrive_detach_disk_image_and_free(vdrive, unit, drive) < 0) {
                    return -1;
                }
                ui_display_drive_current_image(idx, drive, "");
#if 0
                vdrive_device_setup(vdrive, unit, drive);
#endif
            }
            serial_device_type_set(SERIAL_DEVICE_REAL, unit);
            break;
#endif
        default:
            return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------------- */

static void detach_disk_image(disk_image_t *image, vdrive_t *vdrive,
                              unsigned int unit, unsigned int drive)
{
    switch (unit) {
        case 8:     /* fall through */
        case 9:     /* fall through */
        case 10:    /* fall through */
        case 11:
#if 0
            /*
             * TODO: This should not need unit and drive parameters:
             * remembered in vdrive. For now check the consistency.
             */
            if (vdrive->drive != drive || vdrive->unit != unit) {
                log_error(attach_log, "**** detach_disk_image vdrive %u:%u != %u:%u",
                          vdrive->unit, vdrive->drive, unit, drive);
            }
#endif
            machine_drive_image_detach(image, unit, drive);
            drive_image_detach(image, unit, drive);
            vdrive_detach_image(image, unit, drive, vdrive);
            break;
    }
    disk_image_close(image);

#if 0
    if (image != NULL) {
        P64ImageDestroy((PP64Image)image->p64);
        lib_free(image->p64);
    }
#endif
    disk_image_media_destroy(image);
}

static int flush_attached_scp(disk_image_t *image, unsigned int unit,
                              unsigned int drive, int notify)
{
    drive_t *dptr;
    char *recovery = NULL;
    int result;

    if (image == NULL || image->type != DISK_IMAGE_TYPE_SCP || image->read_only) {
        return 0;
    }
    dptr = diskunit_context[unit - 8]->drives[drive];
    if (flux_capture_flags(disk_image_flux_capture_get(image)) & FLUX_CAPTURE_MFM) {
        if (dptr->diskunit->type == DRIVE_TYPE_1581) {
            wd1770_flush_scp(dptr->diskunit);
        } else {
            pc8477_flush_scp(dptr->diskunit);
        }
    } else {
        rotation_rotate_disk(dptr);
        rotation_flux_finish_write(dptr);
    }
    result = fsimage_scp_flush(image, &recovery);
    if (notify) {
        if (result < 0) {
            ui_error("Could not save SCP changes. The disk is still attached.");
        } else if (recovery != NULL) {
            ui_error("The SCP image was not replaced. Saved changes are in %s.", recovery);
        }
    }
    lib_free(recovery);
    return result < 0 ? -1 : 0;
}

static int vdrive_detach_disk_image_and_free(vdrive_t *vdrive,
                                              unsigned int unit,
                                              unsigned int drive)
{
    disk_image_t *image;

    image = vdrive_get_image(vdrive, drive);
    return image != NULL ? detach_disk_image_and_free(image, vdrive, unit, drive) : 0;
}

static int detach_disk_image_and_free(disk_image_t *image, vdrive_t *vdrive,
                                       unsigned int unit, unsigned int drive)
{
    disk_image_t *oldimg;

    oldimg = vdrive_get_image(vdrive, drive);

    if (!oldimg) {
        return 0;
    }
    if (flush_attached_scp(oldimg, unit, drive, 1) < 0) {
        return -1;
    }

    detach_disk_image(image, vdrive, unit, drive);

    if ((image != NULL) && (image == oldimg)) {
        disk_image_destroy(image);
    }
    return 0;
}

int probe_disk_image_type(const char *filename, unsigned int *type)
{
    disk_image_t new_image;
    int rc;

    new_image.gcr = NULL;
    new_image.p64 = lib_calloc(1, sizeof(TP64Image));
    new_image.read_only = 1;

    new_image.device = DISK_IMAGE_DEVICE_FS;

    disk_image_media_create(&new_image);
    disk_image_fsimage_name_set(&new_image, filename);

    new_image.type = DISK_IMAGE_TYPE_UNKNOWN;
    rc = fsimage_open_probe(&new_image);
    if (type != NULL) {
        *type = new_image.type;
    }
    if (rc < 0) {
        P64ImageDestroy((PP64Image) new_image.p64);
        lib_free(new_image.p64);
        disk_image_media_destroy(&new_image);
        return 0;
    }
    P64ImageDestroy((PP64Image) new_image.p64);
    lib_free(new_image.p64);
    disk_image_media_destroy(&new_image);
    return 1;
}

int probe_disk_image(const char *filename)
{
    return probe_disk_image_type(filename, NULL);
}

static int attach_native_image(disk_image_t *image, vdrive_t *vdrive,
                               unsigned int unit, unsigned int drive)
{
    int traps;
    rotation_flux_t checked = {0};
    unsigned int flags = flux_capture_flags(disk_image_flux_capture_get(image));
    drive_t *dptr;
    int ieee;
    int flip;

    if (unit < 8 || unit >= 8 + NUM_DISK_UNITS || drive >= NUM_DRIVES) {
        return -1;
    }
    dptr = diskunit_context[unit - 8]->drives[drive];
    ieee = drive_check_scp_ieee(dptr->diskunit->type);
    if ((drive && (!ieee || !drive_check_dual(dptr->diskunit->type))) || !dptr->true_emulation || drive_check_image_format(DISK_IMAGE_TYPE_SCP, unit - 8) < 0
        || resources_get_int_sprintf("TrapDevice%u", &traps, unit) < 0 || traps
        || rotation_flux_bind(&checked, disk_image_flux_capture_get(image), 0) != FLUX_OK) {
        log_error(attach_log, "SCP requires a 1540, 1541, 1541-II, 1551, 1570, 1571, 1571CR, 1581, 2031, 2040, 3040, 4040, 8050, 8250, SFD-1001, FD-2000 or FD-4000 with true-drive emulation and disabled device traps.");
        return -1;
    }
    {
        unsigned int blank_flags = 0;
        switch (dptr->diskunit->type) {
            case DRIVE_TYPE_1571:
            case DRIVE_TYPE_1571CR:
                if (!drive_flux_align_halftrack(unit - 8)) {
                    blank_flags = FLUX_CAPTURE_DOUBLE_SIDED;
                }
                break;
            case DRIVE_TYPE_8050:
                blank_flags = FLUX_CAPTURE_8X50;
                break;
            case DRIVE_TYPE_8250:
            case DRIVE_TYPE_1001:
                blank_flags = FLUX_CAPTURE_8X50 | FLUX_CAPTURE_DOUBLE_SIDED;
                break;
            case DRIVE_TYPE_1581:
                blank_flags = FLUX_CAPTURE_MFM;
                break;
            case DRIVE_TYPE_2000:
                blank_flags = FLUX_CAPTURE_MFM | FLUX_CAPTURE_MFM_HD;
                break;
            case DRIVE_TYPE_4000:
                blank_flags = FLUX_CAPTURE_MFM | FLUX_CAPTURE_MFM_ED;
                break;
        }
        if (blank_flags & FLUX_CAPTURE_MFM) {
            int density;
            static const unsigned int density_flags[] = {0, 0, FLUX_CAPTURE_MFM_HD, FLUX_CAPTURE_MFM_ED};
            if (resources_get_int_sprintf("Drive%uSCPBlankDensity", &density, unit) == 0 && density > 0) {
                blank_flags = FLUX_CAPTURE_MFM | density_flags[density];
            }
        }
        if (fsimage_scp_prepare_blank(image, blank_flags) != FLUX_OK) {
            return -1;
        }
        flags = flux_capture_flags(disk_image_flux_capture_get(image));
    }
    if ((flags & FLUX_CAPTURE_TOP_ONLY)
        && (dptr->diskunit->type == DRIVE_TYPE_1541 || dptr->diskunit->type == DRIVE_TYPE_1541II)) {
        if (resources_get_int_sprintf("Drive%uSCPFlip", &flip, unit) < 0 || !flip) {
            log_error(attach_log, "Enable the SCP flip option on unit %u to mount this top-side capture on the 1541.", unit);
            return -1;
        }
        if (fsimage_scp_flip_gcr(image) != FLUX_OK) {
            log_error(attach_log, "Could not flip this top-side SCP for 1541 playback.");
            return -1;
        }
        flags = flux_capture_flags(disk_image_flux_capture_get(image));
    }
    if ((flags & FLUX_CAPTURE_MFM)
        ? !drive_check_scp_mfm(dptr->diskunit->type, flags)
        : (!drive_check_scp_gcr(dptr->diskunit->type, flags)
           || drive_check_scp_mfm(dptr->diskunit->type, FLUX_CAPTURE_MFM))) {
        log_error(attach_log, "SCP capture encoding or density is incompatible with the configured drive.");
        return -1;
    }
    if (!ieee && (flags & FLUX_CAPTURE_DOUBLE_SIDED)
        && (drive_get_num_heads(dptr->diskunit->type) < 2 || drive_flux_align_halftrack(unit - 8))) {
        log_error(attach_log, "Double-sided GCR SCP requires a 1571 or 1571CR with flux alignment disabled.");
        return -1;
    }
    if (detach_disk_image_and_free(vdrive_get_image(vdrive, drive), vdrive, unit, drive) < 0) {
        return -1;
    }
    if (drive_image_attach(image, unit, drive) < 0) {
        return -1;
    }
    if (ieee || (flags & FLUX_CAPTURE_MFM)) {
        if (machine_drive_image_attach(image, unit, drive) < 0) {
            machine_drive_image_detach(image, unit, drive);
            drive_image_detach(image, unit, drive);
            return -1;
        }
    }
    if (vdrive_attach_image(image, unit, drive, vdrive) < 0) {
        if (ieee || (flags & FLUX_CAPTURE_MFM)) {
            machine_drive_image_detach(image, unit, drive);
        }
        drive_image_detach(image, unit, drive);
        return -1;
    }
    return 0;
}

int file_system_attach_flux_capture(unsigned int unit, unsigned int drive,
                                     flux_capture_t *capture, int read_only, int scaled)
{
    disk_image_t *image;
    vdrive_t *vdrive;
    int rc;
    if (unit < 8 || unit >= 8 + NUM_DISK_UNITS || drive >= NUM_DRIVES
        || !flux_capture_is_sealed(capture)) {
        return -1;
    }
    vdrive = file_system_get_vdrive(unit);
    if (vdrive == NULL || disk_image_flux_capture_get(vdrive_get_image(vdrive, drive)) == capture) {
        return -1;
    }
    image = disk_image_create();
    image->device = DISK_IMAGE_DEVICE_FS;
    image->type = DISK_IMAGE_TYPE_SCP;
    image->read_only = read_only;
    image->tracks = FLUX_CAPTURE_TRACKS / 2;
    image->max_half_tracks = FLUX_CAPTURE_TRACKS;
    if (flux_capture_flags(capture) & FLUX_CAPTURE_8X50) {
        image->tracks = (flux_capture_flags(capture) & FLUX_CAPTURE_DOUBLE_SIDED) ? 154 : 77;
        image->max_half_tracks = 154;
    }
    image->gcr = NULL;
    disk_image_media_create(image);
    disk_image_fsimage_name_set(image, "Embedded native flux");
    image->media.fsimage->flux = capture;
    image->media.fsimage->flux_only = FSIMAGE_FLUX_EMBEDDED;
    rc = fsimage_scp_prepare_embedded_write(image, scaled,
        drive_check_scp_write(diskunit_context[unit - 8]->type, 0)
        || drive_check_scp_ieee(diskunit_context[unit - 8]->type));
    if (!rc) {
        rc = attach_native_image(image, vdrive, unit, drive);
    }
    if (rc < 0) {
        image->media.fsimage->flux = NULL; /* Caller retains ownership. */
        disk_image_media_destroy(image);
        disk_image_destroy(image);
        return -1;
    }
    serial_device_type_set(SERIAL_DEVICE_VIRT, unit);
    file_system_set_serial_hooks(unit, ATTACH_DEVICE_NONE);
    ui_display_drive_current_image(unit - 8, drive, "Embedded native flux");
    return 0;
}

static int attach_disk_image(disk_image_t *oldimage, vdrive_t *vdrive,
                             const char *filename, unsigned int unit,
                             unsigned int drive, int devicetype)
{
    disk_image_t *image;
    disk_image_t new_image;
    int err = -1;
    int test_unit;

    if (filename == NULL) {
        log_error(attach_log, "No name, cannot attach floppy image.");
        return -1;
    }

    /* Make sure that we aren't attaching a disk image that is already
     * attached */
    for (test_unit = 8; test_unit < 8 + NUM_DISK_UNITS; ++test_unit) {
        int test_drive;
        for (test_drive = 0; test_drive < NUM_DRIVES; ++test_drive) {
            /* It's OK to replace ourselves with the same disk */
            if (unit != test_unit || drive != test_drive) {
                const char *test_name = file_system_get_disk_name(test_unit, test_drive);
                if (test_name && archdep_real_path_equal(test_name, filename)) {
                    log_error(attach_log, "`%s' is already mounted on drive %d:%d", filename, test_unit, test_drive);
                    return -1;
                }
            }
        }
    }

    new_image.gcr = NULL;
    new_image.p64 = lib_calloc(1, sizeof(TP64Image));
    new_image.read_only = (unsigned int)attach_device_readonly_enabled[unit - 8][drive];

    switch (devicetype) {
        case ATTACH_DEVICE_NONE:
        case ATTACH_DEVICE_VIRT:
        case ATTACH_DEVICE_FS:
            new_image.device = DISK_IMAGE_DEVICE_FS;
            break;
    }

    disk_image_media_create(&new_image);

    switch (devicetype) {
        case ATTACH_DEVICE_NONE:
        case ATTACH_DEVICE_VIRT:
        case ATTACH_DEVICE_FS:
            disk_image_fsimage_name_set(&new_image, filename);
            break;
    }

    if (flush_attached_scp(oldimage, unit, drive, 1) < 0 || disk_image_open(&new_image) < 0) {
        P64ImageDestroy((PP64Image) new_image.p64);
        lib_free(new_image.p64);
        disk_image_media_destroy(&new_image);
        return -1;
    }

    if (new_image.type == DISK_IMAGE_TYPE_SCP) {
        if (!attach_device_readonly_enabled[unit - 8][drive]) {
            const flux_capture_t *capture = disk_image_flux_capture_get(&new_image);
            if (flux_capture_revolutions(capture) == 1) {
                if (new_image.read_only) {
                    fsimage_scp_enable_write(&new_image, 0, SCP_WRITE_CAPTURE_TIMING, 0, 0);
                }
            } else if (scp_multirev_write) {
                unsigned int type = diskunit_context[unit - 8]->type;
                scp_write_clock_t clock = SCP_WRITE_CAPTURE_TIMING;
                int flip = 0;
                int gcr_window_alignment = drive_check_scp_write(type, 0);
                if (gcr_window_alignment) {
                    clock = drive_flux_speed_mode(unit - 8) == 1
                        ? SCP_WRITE_MEASURE_CAPTURE_CLOCK : SCP_WRITE_MEASURE_AUTO_CLOCK;
                } else if (drive_check_scp_ieee(type)) {
                    gcr_window_alignment = 1;
                    clock = SCP_WRITE_MEASURE_AUTO_CLOCK;
                }
                if ((type == DRIVE_TYPE_1541 || type == DRIVE_TYPE_1541II)
                    && (flux_capture_flags(capture) & FLUX_CAPTURE_TOP_ONLY)) {
                    resources_get_int_sprintf("Drive%uSCPFlip", &flip, unit);
                }
                fsimage_scp_enable_write(&new_image, 1, clock, flip, gcr_window_alignment);
            }
        }
        image = disk_image_create();
        memcpy(image, &new_image, sizeof(*image));
        lib_free(new_image.p64);
        image->p64 = NULL;
        err = attach_native_image(image, vdrive, unit, drive);
        if (err < 0) {
            disk_image_media_destroy(image);
            disk_image_destroy(image);
        }
        return err;
    }

    if (detach_disk_image_and_free(oldimage, vdrive, unit, drive) < 0) {
        P64ImageDestroy((PP64Image)new_image.p64);
        lib_free(new_image.p64);
        disk_image_media_destroy(&new_image);
        return -1;
    }

    image = disk_image_create();

    memcpy(image, &new_image, sizeof(disk_image_t));
    /* free the P64 stuff, fixes the leak in src/attach.c, reported when
     * using --enable-debug */
    lib_free(new_image.p64);

    switch (unit) {
        case 8:
        case 9:
        case 10:
        case 11:
            /* "wired OR". If any of the three succeeds, err becomes 0 */
            err = drive_image_attach(image, unit, drive);
            err &= vdrive_attach_image(image, unit, drive, vdrive);
            err &= machine_drive_image_attach(image, unit, drive);
            break;
    }
    if (err) {
        disk_image_close(image);
#if 0
        P64ImageDestroy((PP64Image)image->p64);
        lib_free(image->p64);
#endif
        disk_image_media_destroy(image);
        disk_image_destroy(image);
    }
    return err;
}

/* ------------------------------------------------------------------------- */

static int file_system_attach_disk_internal(unsigned int unit, unsigned int drive,
                                            const char *filename)
{
    vdrive_t *vdrive;
    disk_image_t *image;

    vdrive = file_system_get_vdrive(unit);
    image = vdrive_get_image(vdrive, drive);
    serial_device_type_set(SERIAL_DEVICE_VIRT, unit);

    if (attach_disk_image(image, vdrive, filename, unit, drive,
                          file_system_device_enabled[unit - 8]) < 0) {
        return -1;
    } else {
        log_message(attach_log, "Disk image unit %u:%u %s", unit, drive, filename);
        file_system_set_serial_hooks(unit, ATTACH_DEVICE_NONE);
        fliplist_set_current(unit, filename);
        ui_display_drive_current_image(unit - 8, drive, filename);
    }

    image = vdrive_get_image(vdrive, drive);
    if (image) {
        event_record_attach_image(unit, drive, filename, image->read_only);
    }

    return 0;
}

int file_system_attach_disk(unsigned int unit, unsigned int drive, const char *filename)
{
    if (event_playback_active()) {
        return -1;
    }

    /* TODO: drive 1? */
    if (network_connected() && drive == 0) {
        network_attach_image(unit, filename);
        return 0;
    }

    return file_system_attach_disk_internal(unit, drive, filename);
}

static int file_system_detach_disk_single(unsigned int unit, unsigned int drive)
{
    vdrive_t *vdrive;
    disk_image_t *image;

    vdrive = file_system_get_vdrive(unit);
    image = vdrive_get_image(vdrive, drive);
    if (image) {
        if (detach_disk_image_and_free(image, vdrive, unit, drive) < 0) {
            return -1;
        }
        ui_display_drive_current_image(unit - 8, drive, "");
    }
    return 0;
}

static int file_system_detach_disk_internal(unsigned int unit, unsigned int drive)
{
    char event_data[2];

    if ((unit >= 8) && (unit < 8 + NUM_DISK_UNITS)) {
        if (file_system_detach_disk_single(unit, drive) < 0) {
            return -1;
        }
        file_system_set_serial_hooks(unit, ATTACH_DEVICE_FS);
    } else {
        log_error(attach_log, "Cannot detach unit %u drive %u.", unit, drive);
        return -1;
    }

    /* TODO: drive 1 for EVENT_ATTACHDISK */
    event_data[0] = (char)unit;
    event_data[1] = 0;

    event_record(EVENT_ATTACHDISK, (void *)event_data, 2);
    return 0;
}

int file_system_detach_disk_checked(unsigned int unit, unsigned int drive)
{
    char event_data[2];

    if (event_playback_active()) {
        return -1;
    }

    event_data[0] = (char)unit;
    event_data[1] = 0;

    if (network_connected()) {
        network_event_record(EVENT_ATTACHDISK, (void *)event_data, 2);
        return -1;
    }

    return file_system_detach_disk_internal(unit, drive);
}

void file_system_detach_disk(unsigned int unit, unsigned int drive)
{
    file_system_detach_disk_checked(unit, drive);
}

void file_system_detach_disk_all(void)
{
    int unit;

    for (unit = DRIVE_UNIT_MIN; unit <= DRIVE_UNIT_MAX; unit++) {
        /* detaching will restore the vdrive hooks, so we must test if vdrive
           is not NULL */
        if (file_system_get_vdrive(unit) != NULL) {
            file_system_detach_disk(unit, 0);
            file_system_detach_disk(unit, 1);
        }
    }
}

void file_system_detach_disk_shutdown(void)
{
    vdrive_t *vdrive;
    unsigned int i, j;
    disk_image_t *image;

    for (i = 0; i < NUM_DISK_UNITS; i++) {
        if (file_system_device_enabled[i] == ATTACH_DEVICE_REAL) {
            serial_realdevice_disable();
        } else {
            vdrive = file_system_get_vdrive(i + 8);
            for (j = 0; j < NUM_DRIVES; j++) {
                image = vdrive_get_image(vdrive, j);
                if (image && flush_attached_scp(image, i + 8, j, 0) == 0) {
                    detach_disk_image_and_free(image, vdrive, i + 8, j);
                }
            }
        }
    }
}

void file_system_event_playback(unsigned int unit, unsigned int drive, const char *filename)
{
    if (filename == NULL || filename[0] == 0) {
        file_system_detach_disk_internal(unit, drive);
    } else {
        file_system_attach_disk_internal(unit, drive, filename);
    }
}
