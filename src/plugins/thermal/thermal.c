/**
 * collectd - src/thermal.c
 * Copyright (C) 2008  Michał Mirosław
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
 * Authors:
 *   Michał Mirosław <mirq-linux at rere.qmqm.pl>
 **/

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"
#include "utils/ignorelist/ignorelist.h"

#if !KERNEL_LINUX
#error "This module is for Linux only."
#endif

static const char *config_keys[] = {"Device", "IgnoreSelected",
                                    "ForceUseProcfs"};

static const char *const dirname_sysfs = "/sys/class/thermal";
static const char *const dirname_procfs = "/proc/acpi/thermal_zone";

static bool force_procfs;
static ignorelist_t *device_list;

enum thermal_fs_e {
  THERMAL_NONE = 0,
  THERMAL_PROCFS,
  THERMAL_SYSFS,
};

static enum thermal_fs_e thermal_fs;

enum dev_type { TEMP = 0, COOLING_DEV };

enum {
  FAM_COOLING_DEVICE_MAX_STATE = 0,
  FAM_COOLING_DEVICE_CUR_STATE,
  FAM_THERMAL_ZONE_CELSIUS,
  FAM_THERMAL_MAX,
};

static int thermal_sysfs_device_read(const char __attribute__((unused)) * dir,
                                     const char *name, void *user_data)
{
  char filename[PATH_MAX];
  bool success = false;
  value_t value;

  if (device_list && ignorelist_match(device_list, name))
    return -1;

  metric_family_t *fams = user_data;

  snprintf(filename, sizeof(filename), "%s/%s/temp", dirname_sysfs, name);
  if (parse_value_file(filename, &value, DS_TYPE_GAUGE) == 0) {
    value.gauge /= 1000.0;
    metric_family_append(&fams[FAM_THERMAL_ZONE_CELSIUS], "zone", name, value,
                         NULL);
    success = true;
  }

  snprintf(filename, sizeof(filename), "%s/%s/cur_state", dirname_sysfs, name);
  if (parse_value_file(filename, &value, DS_TYPE_GAUGE) == 0) {
    metric_family_append(&fams[FAM_COOLING_DEVICE_CUR_STATE], "device", name,
                         value, NULL);
    success = true;
  }

  snprintf(filename, sizeof(filename), "%s/%s/max_state", dirname_sysfs, name);
  if (parse_value_file(filename, &value, DS_TYPE_GAUGE) == 0) {
    metric_family_append(&fams[FAM_COOLING_DEVICE_MAX_STATE], "device", name,
                         value, NULL);
    success = true;
  }

  return success ? 0 : -1;
}

static int thermal_procfs_device_read(const char __attribute__((unused)) * dir,
                                      const char *name, void *user_data)
{
  const char str_temp[] = "temperature:";
  char filename[256];
  char data[1024];
  int len;

  if (device_list && ignorelist_match(device_list, name))
    return -1;

  metric_family_t *fams = user_data;
  /**
   * rechot ~ # cat /proc/acpi/thermal_zone/THRM/temperature
   * temperature:             55 C
   */

  len = snprintf(filename, sizeof(filename), "%s/%s/temperature",
                 dirname_procfs, name);
  if ((len < 0) || ((size_t)len >= sizeof(filename)))
    return -1;

  len = (ssize_t)read_text_file_contents(filename, data, sizeof(data));
  if ((len > 0) && ((size_t)len > sizeof(str_temp)) && (data[--len] == '\n') &&
      (!strncmp(data, str_temp, sizeof(str_temp) - 1))) {
    char *endptr = NULL;
    double temp;
    double factor, add;

    if (data[--len] == 'C') {
      add = 0;
      factor = 1.0;
    } else if (data[len] == 'F') {
      add = -32;
      factor = 5.0 / 9.0;
    } else if (data[len] == 'K') {
      add = -273.15;
      factor = 1.0;
    } else
      return -1;

    while (len > 0 && data[--len] == ' ')
      ;
    data[len + 1] = 0;

    while (len > 0 && data[--len] != ' ')
      ;
    ++len;

    errno = 0;
    temp = (strtod(data + len, &endptr) + add) * factor;

    if (endptr != data + len && errno == 0) {
      metric_family_append(&fams[FAM_THERMAL_ZONE_CELSIUS], "zone", name,
                           (value_t){.gauge = temp}, NULL);
      return 0;
    }
  }

  return -1;
}

static int thermal_config(const char *key, const char *value)
{
  if (device_list == NULL)
    device_list = ignorelist_create(1);

  if (strcasecmp(key, "Device") == 0) {
    if (ignorelist_add(device_list, value)) {
      ERROR("thermal plugin: "
            "Cannot add value to ignorelist.");
      return 1;
    }
  } else if (strcasecmp(key, "IgnoreSelected") == 0) {
    ignorelist_set_invert(device_list, 1);
    if (IS_TRUE(value))
      ignorelist_set_invert(device_list, 0);
  } else if (strcasecmp(key, "ForceUseProcfs") == 0) {
    force_procfs = false;
    if (IS_TRUE(value))
      force_procfs = true;
  } else {
    return -1;
  }

  return 0;
}

static int thermal_read(void)
{
  metric_family_t fams[FAM_THERMAL_MAX] = {
      [FAM_COOLING_DEVICE_MAX_STATE] =
          {
              .name = "host_cooling_device_max_state",
              .type = METRIC_TYPE_GAUGE,
          },
      [FAM_COOLING_DEVICE_CUR_STATE] =
          {
              .name = "host_cooling_device_cur_state",
              .type = METRIC_TYPE_GAUGE,
          },
      [FAM_THERMAL_ZONE_CELSIUS] =
          {
              .name = "host_thermal_zone_celsius",
              .type = METRIC_TYPE_GAUGE,
          },
  };

  if (thermal_fs == THERMAL_PROCFS) {
    walk_directory(dirname_procfs, thermal_procfs_device_read, fams, 0);
  } else {
    walk_directory(dirname_sysfs, thermal_sysfs_device_read, fams, 0);
  }

  for (size_t i = 0; i < FAM_THERMAL_MAX; i++) {
    if (fams[i].metric.num > 0) {
      int status = plugin_dispatch_metric_family(&fams[i]);
      if (status != 0) {
        ERROR("thremal plugin: plugin_dispatch_metric_family failed: %s",
              STRERROR(status));
      }
      metric_family_metric_reset(&fams[i]);
    }
  }

  return 0;
}

static int thermal_init(void)
{
  int ret = -1;

  if (!force_procfs && access(dirname_sysfs, R_OK | X_OK) == 0) {
    thermal_fs = THERMAL_SYSFS;
    ret = plugin_register_read("thermal", thermal_read);
  } else if (access(dirname_procfs, R_OK | X_OK) == 0) {
    thermal_fs = THERMAL_PROCFS;
    ret = plugin_register_read("thermal", thermal_read);
  }

  return ret;
}

static int thermal_shutdown(void)
{
  ignorelist_free(device_list);

  return 0;
}

void module_register(void)
{
  plugin_register_config("thermal", thermal_config, config_keys, STATIC_ARRAY_SIZE(config_keys));
  plugin_register_init("thermal", thermal_init);
  plugin_register_shutdown("thermal", thermal_shutdown);
}
