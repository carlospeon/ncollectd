/**
 * collectd - src/ethstat.c
 * Copyright (C) 2011       Cyril Feraudet
 * Copyright (C) 2012       Florian "octo" Forster
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
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
 *   Cyril Feraudet <cyril at feraudet.com>
 *   Florian "octo" Forster <octo@collectd.org>
 **/

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"

#if HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif
#if HAVE_NET_IF_H
#include <net/if.h>
#endif
#if HAVE_LINUX_SOCKIOS_H
#include <linux/sockios.h>
#endif
#if HAVE_LINUX_ETHTOOL_H
#include <linux/ethtool.h>
#endif

static char **interfaces;
static size_t interfaces_num;

static int ethstat_add_interface(const oconfig_item_t *ci)
{
  char **tmp;
  int status;

  tmp = realloc(interfaces, sizeof(*interfaces) * (interfaces_num + 1));
  if (tmp == NULL)
    return -1;
  interfaces = tmp;
  interfaces[interfaces_num] = NULL;

  status = cf_util_get_string(ci, interfaces + interfaces_num);
  if (status != 0)
    return status;

  interfaces_num++;
  INFO("ethstat plugin: Registered interface %s", interfaces[interfaces_num - 1]);

  return 0;
}

static int ethstat_config(oconfig_item_t *ci)
{
  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;

    if (strcasecmp("Interface", child->key) == 0)
      ethstat_add_interface(child);
    else
      WARNING("ethstat plugin: The config option \"%s\" is unknown.", child->key);
  }

  return 0;
}

static void ethstat_submit_value(const char *device, const char *name,
                                 counter_t value)
{
  char fam_name[256];
  ssnprintf(fam_name, sizeof(fam_name), "host_ethstat_%s_total", name);

  metric_family_t fam = {
    .name = fam_name,
    .type = METRIC_TYPE_COUNTER,
  };

  metric_family_append(&fam, "device", device, (value_t){.counter = value}, NULL);

  int status = plugin_dispatch_metric_family(&fam);
  if (status != 0)
    ERROR("ethstat plugin: plugin_dispatch_metric_family failed: %s", STRERROR(status));

  metric_family_metric_reset(&fam);
}

static int ethstat_read_interface(char *device)
{
  int fd;
  struct ethtool_gstrings *strings;
  struct ethtool_stats *stats;
  size_t n_stats;
  size_t strings_size;
  size_t stats_size;
  int status;

  fd = socket(AF_INET, SOCK_DGRAM, /* protocol = */ 0);
  if (fd < 0) {
    ERROR("ethstat plugin: Failed to open control socket: %s", STRERRNO);
    return 1;
  }

  struct ethtool_drvinfo drvinfo = {.cmd = ETHTOOL_GDRVINFO};

  struct ifreq req = {.ifr_data = (void *)&drvinfo};

  sstrncpy(req.ifr_name, device, sizeof(req.ifr_name));

  status = ioctl(fd, SIOCETHTOOL, &req);
  if (status < 0) {
    close(fd);
    ERROR("ethstat plugin: Failed to get driver information "
          "from %s: %s",
          device, STRERRNO);
    return -1;
  }

  n_stats = (size_t)drvinfo.n_stats;
  if (n_stats < 1) {
    close(fd);
    ERROR("ethstat plugin: No stats available for %s", device);
    return -1;
  }

  strings_size = sizeof(struct ethtool_gstrings) + (n_stats * ETH_GSTRING_LEN);
  stats_size = sizeof(struct ethtool_stats) + (n_stats * sizeof(uint64_t));

  strings = malloc(strings_size);
  stats = malloc(stats_size);
  if ((strings == NULL) || (stats == NULL)) {
    close(fd);
    sfree(strings);
    sfree(stats);
    ERROR("ethstat plugin: malloc failed.");
    return -1;
  }

  strings->cmd = ETHTOOL_GSTRINGS;
  strings->string_set = ETH_SS_STATS;
  strings->len = n_stats;
  req.ifr_data = (void *)strings;
  status = ioctl(fd, SIOCETHTOOL, &req);
  if (status < 0) {
    close(fd);
    free(strings);
    free(stats);
    ERROR("ethstat plugin: Cannot get strings from %s: %s", device, STRERRNO);
    return -1;
  }

  stats->cmd = ETHTOOL_GSTATS;
  stats->n_stats = n_stats;
  req.ifr_data = (void *)stats;
  status = ioctl(fd, SIOCETHTOOL, &req);
  if (status < 0) {
    close(fd);
    free(strings);
    free(stats);
    ERROR("ethstat plugin: Reading statistics from %s failed: %s", device,
          STRERRNO);
    return -1;
  }

  for (size_t i = 0; i < n_stats; i++) {
    char *stat_name;

    stat_name = (void *)&strings->data[i * ETH_GSTRING_LEN];
    /* Remove leading spaces in key name */
    while (isspace((int)*stat_name))
      stat_name++;

    DEBUG("ethstat plugin: device = \"%s\": %s = %" PRIu64, device, stat_name,
          (uint64_t)stats->data[i]);
    ethstat_submit_value(device, stat_name, (counter_t)stats->data[i]);
  }

  close(fd);
  sfree(strings);
  sfree(stats);

  return 0;
}

static int ethstat_read(void)
{
  for (size_t i = 0; i < interfaces_num; i++)
    ethstat_read_interface(interfaces[i]);

  return 0;
}

static int ethstat_shutdown(void)
{
  if (interfaces == NULL)
    return 0;

  for (size_t i = 0; i < interfaces_num; i++)
    sfree(interfaces[i]);

  sfree(interfaces);
  return 0;
}

void module_register(void)
{
  plugin_register_complex_config("ethstat", ethstat_config);
  plugin_register_read("ethstat", ethstat_read);
  plugin_register_shutdown("ethstat", ethstat_shutdown);
}
