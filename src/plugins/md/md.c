/**
 * collectd - src/md.c
 * Copyright (C) 2010,2011  Michael Hanselmann
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; only version 2 of the License is applicable.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Author:
 *   Michael Hanselmann
 **/

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"
#include "utils/ignorelist/ignorelist.h"

#include <sys/ioctl.h>

#include <linux/major.h>
#include <linux/raid/md_u.h>

#ifdef HAVE_SYS_SYSMACROS_H
#include <sys/sysmacros.h>
#endif

#define PROC_DISKSTATS "/proc/diskstats"
#define DEV_DIR "/dev"

static const char *config_keys[] = {"Device", "IgnoreSelected"};
static int config_keys_num = STATIC_ARRAY_SIZE(config_keys);

static ignorelist_t *ignorelist;

enum {
  FAM_MD_ACTIVE = 0,
  FAM_MD_FAILED,
  FAM_MD_SPARE,
  FAM_MD_MISSING,
  FAM_MD_MAX,
};

static int md_config(const char *key, const char *value) {
  if (ignorelist == NULL)
    ignorelist = ignorelist_create(/* invert = */ 1);
  if (ignorelist == NULL)
    return 1;

  if (strcasecmp(key, "Device") == 0) {
    ignorelist_add(ignorelist, value);
  } else if (strcasecmp(key, "IgnoreSelected") == 0) {
    ignorelist_set_invert(ignorelist, IS_TRUE(value) ? 0 : 1);
  } else {
    return -1;
  }

  return 0;
}

static void md_process(metric_family_t *fams, const int minor,
                       const char *path) {
  int fd;
  struct stat st;
  mdu_array_info_t array;

  fd = open(path, O_RDONLY);
  if (fd < 0) {
    WARNING("md: open(%s): %s", path, STRERRNO);
    return;
  }

  if (fstat(fd, &st) < 0) {
    WARNING("md: Unable to fstat file descriptor for %s: %s", path, STRERRNO);
    close(fd);
    return;
  }

  if (!S_ISBLK(st.st_mode)) {
    WARNING("md: %s is no block device", path);
    close(fd);
    return;
  }

  if (st.st_rdev != makedev(MD_MAJOR, minor)) {
    WARNING("md: Major/minor of %s are %i:%i, should be %i:%i", path,
            (int)major(st.st_rdev), (int)minor(st.st_rdev), (int)MD_MAJOR,
            minor);
    close(fd);
    return;
  }

  /* Retrieve md information */
  if (ioctl(fd, GET_ARRAY_INFO, &array) < 0) {
    WARNING("md: Unable to retrieve array info from %s: %s", path, STRERRNO);
    close(fd);
    return;
  }

  close(fd);

  /*
   * The mdu_array_info_t structure contains numbers of disks in the array.
   * However, disks are accounted for more than once:
   *
   * active:  Number of active (in sync) disks.
   * spare:   Number of stand-by disks.
   * working: Number of working disks. (active + sync)
   * failed:  Number of failed disks.
   * nr:      Number of physically present disks. (working + failed)
   * raid:    Number of disks in the RAID. This may be larger than "nr" if
   *          disks are missing and smaller than "nr" when spare disks are
   *          around.
   */
  metric_t m = {0};
  metric_label_set(&m, "device", path);
  char minor_buffer[21];
  ssnprintf(minor_buffer, sizeof(minor_buffer), "%i", minor);
  metric_label_set(&m, "minor", minor_buffer);

  m.value.gauge = (gauge_t)array.active_disks;
  metric_family_metric_append(&fams[FAM_MD_ACTIVE], m);

  m.value.gauge = (gauge_t)array.failed_disks;
  metric_family_metric_append(&fams[FAM_MD_FAILED], m);

  m.value.gauge = (gauge_t)array.spare_disks;
  metric_family_metric_append(&fams[FAM_MD_SPARE], m);

  gauge_t disks_missing = 0.0;
  if (array.raid_disks > array.nr_disks)
    disks_missing = (gauge_t)(array.raid_disks - array.nr_disks);

  m.value.gauge = disks_missing;
  metric_family_metric_append(&fams[FAM_MD_MISSING], m);

  metric_reset(&m);
} /* void md_process */

static int md_read(void) {
  FILE *fh;
  char buffer[1024];
  metric_family_t fams[FAM_MD_MAX] = {
      [FAM_MD_ACTIVE] =
          {
              .name = "host_md_active",
              .type = METRIC_TYPE_GAUGE,
          },
      [FAM_MD_FAILED] =
          {
              .name = "host_md_failed",
              .type = METRIC_TYPE_GAUGE,
          },
      [FAM_MD_SPARE] =
          {
              .name = "host_md_spare",
              .type = METRIC_TYPE_GAUGE,
          },
      [FAM_MD_MISSING] =
          {
              .name = "host_md_missing",
              .type = METRIC_TYPE_GAUGE,
          },
  };

  fh = fopen(PROC_DISKSTATS, "r");
  if (fh == NULL) {
    WARNING("md: Unable to open %s: %s", PROC_DISKSTATS, STRERRNO);
    return -1;
  }

  /* Iterate md devices */
  while (fgets(buffer, sizeof(buffer), fh) != NULL) {
    char path[PATH_MAX];
    char *fields[4];
    char *name;
    int major, minor;

    /* Extract interesting fields */
    if (strsplit(buffer, fields, STATIC_ARRAY_SIZE(fields)) < 3)
      continue;

    major = atoi(fields[0]);

    if (major != MD_MAJOR)
      continue;

    minor = atoi(fields[1]);
    name = fields[2];

    if (ignorelist_match(ignorelist, name))
      continue;

    /* FIXME: Don't hardcode path. Walk /dev collecting major,
     * minor and name, then use lookup table to find device.
     * Alternatively create a temporary device file with correct
     * major/minor, but that again can be tricky if the filesystem
     * with the device file is mounted using the "nodev" option.
     */
    snprintf(path, sizeof(path), "%s/%s", DEV_DIR, name);

    md_process(fams, minor, path);
  }

  fclose(fh);

  for (size_t i = 0; i < FAM_MD_MAX; i++) {
    if (fams[i].metric.num > 0) {
      int status = plugin_dispatch_metric_family(&fams[i]);
      if (status != 0) {
        ERROR("serial plugin: plugin_dispatch_metric_family failed: %s",
              STRERROR(status));
      }
      metric_family_metric_reset(&fams[i]);
    }
  }

  return 0;
} /* int md_read */

void module_register(void) {
  plugin_register_config("md", md_config, config_keys, config_keys_num);
  plugin_register_read("md", md_read);
} /* void module_register */
