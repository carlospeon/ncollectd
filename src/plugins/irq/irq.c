/**
 * collectd - src/irq.c
 * Copyright (C) 2007  Peter Holik
 * Copyright (C) 2011  Florian Forster
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
 *   Peter Holik <peter at holik.at>
 **/

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"
#include "utils/ignorelist/ignorelist.h"

#if !KERNEL_LINUX && !KERNEL_NETBSD
#error "No applicable input method."
#endif

#if KERNEL_NETBSD
#include <malloc.h>
#include <sys/evcnt.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#endif /* KERNEL_NETBSD */

static const char *config_keys[] = {"Irq", "IgnoreSelected"};
static int config_keys_num = STATIC_ARRAY_SIZE(config_keys);

static ignorelist_t *ignorelist;

static int irq_config(const char *key, const char *value)
{
  if (ignorelist == NULL)
    ignorelist = ignorelist_create(/* invert = */ 1);

  if (strcasecmp(key, "Irq") == 0) {
    ignorelist_add(ignorelist, value);
  } else if (strcasecmp(key, "IgnoreSelected") == 0) {
    int invert = 1;
    if (IS_TRUE(value))
      invert = 0;
    ignorelist_set_invert(ignorelist, invert);
  } else {
    return -1;
  }

  return 0;
}

#if KERNEL_LINUX
static int irq_read_data(metric_family_t *fam)
{
  /*
   * Example content:
   *         CPU0       CPU1       CPU2       CPU3
   * 0:       2574          1          3          2   IO-APIC-edge      timer
   * 1:     102553     158669     218062      70587   IO-APIC-edge      i8042
   * 8:          0          0          0          1   IO-APIC-edge      rtc0
   */
  FILE *fh = fopen("/proc/interrupts", "r");
  if (fh == NULL) {
    ERROR("irq plugin: fopen (/proc/interrupts): %s", STRERRNO);
    return -1;
  }

  /* Get CPU count from the first line */
  char cpu_buffer[1024];
  char *cpu_fields[256];
  int cpu_count;

  if (fgets(cpu_buffer, sizeof(cpu_buffer), fh) != NULL) {
    cpu_count = strsplit(cpu_buffer, cpu_fields, STATIC_ARRAY_SIZE(cpu_fields));
    for (int i = 0; i < cpu_count; i++) {
      if (strncmp(cpu_fields[i], "CPU", 3) == 0)
        cpu_fields[i] += 3;
    }
  } else {
    ERROR("irq plugin: unable to get CPU count from first line "
          "of /proc/interrupts");
    fclose(fh);
    return -1;
  }

  metric_t m = {0};
  char buffer[1024];
  char *fields[256];

  while (fgets(buffer, sizeof(buffer), fh) != NULL) {
    int fields_num = strsplit(buffer, fields, STATIC_ARRAY_SIZE(fields));
    if (fields_num < 2)
      continue;

    /* Parse this many numeric fields, skip the rest
     * (+1 because first there is a name of irq in each line) */
    int irq_values_to_parse;
    if (fields_num >= cpu_count + 1)
      irq_values_to_parse = cpu_count;
    else
      irq_values_to_parse = fields_num - 1;

    /* First field is irq name and colon */
    char *irq_name = fields[0];
    size_t irq_name_len = strlen(irq_name);
    if (irq_name_len < 2)
      continue;

    /* Check if irq name ends with colon.
     * Otherwise it's a header. */
    if (irq_name[irq_name_len - 1] != ':')
      continue;

    /* Is it the the ARM fast interrupt (FIQ)? */
    if (irq_name_len == 4 && (strncmp(irq_name, "FIQ:", 4) == 0))
      continue;

    irq_name[irq_name_len - 1] = '\0';
    irq_name_len--;

    if (ignorelist_match(ignorelist, irq_name) != 0)
      continue;

    metric_label_set(&m, "irq", irq_name);

    for (int i = 1; i <= irq_values_to_parse; i++) {
      /* Per-CPU value */
      value_t v;
      int status = parse_value(fields[i], &v, DS_TYPE_COUNTER);
      if (status != 0)
        break;
      metric_label_set(&m, "cpu", cpu_fields[i - 1]);
      m.value.counter = v.counter;
      metric_family_metric_append(fam, m);
    } /* for (i) */
  }
  metric_reset(&m);

  fclose(fh);

  return 0;
}
#endif /* KERNEL_LINUX */

#if KERNEL_NETBSD
static int irq_read_data(metric_family_t *fam)
{
  const int mib[4] = {CTL_KERN, KERN_EVCNT, EVCNT_TYPE_INTR,
                      KERN_EVCNT_COUNT_NONZERO};
  size_t buflen = 0;
  void *buf = NULL;
  const struct evcnt_sysctl *evs, *last_evs;

  for (;;) {
    size_t newlen;
    int error;

    newlen = buflen;
    if (buflen)
      buf = malloc(buflen);
    error = sysctl(mib, __arraycount(mib), buf, &newlen, NULL, 0);
    if (error) {
      ERROR("irq plugin: failed to get event count");
      return -1;
    }
    if (newlen <= buflen) {
      buflen = newlen;
      break;
    }
    if (buf)
      free(buf);
    buflen = newlen;
  }
  evs = buf;
  last_evs = (void *)((char *)buf + buflen);
  buflen /= sizeof(uint64_t);
  metric_t m = {0};
  while (evs < last_evs && buflen > sizeof(*evs) / sizeof(uint64_t) &&
         buflen >= evs->ev_len) {
    char irqname[80];

    snprintf(irqname, 80, "%s-%s", evs->ev_strings,
             evs->ev_strings + evs->ev_grouplen + 1);

    if (ignorelist_match(ignorelist, irqname) == 0) {
      metric_label_set(&m, "irq", irqname);
      m.value.counter = evs->ev_count;
      metric_family_metric_append(fam, m);
    }

    buflen -= evs->ev_len;
    evs = (const void *)((const uint64_t *)evs + evs->ev_len);
  }
  free(buf);
  metric_reset(&m);
  return 0;
}
#endif /* KERNEL_NETBSD */

static int irq_read(void)
{
  metric_family_t fam = {
    .name = "host_interrupts_total",
    .type = METRIC_TYPE_COUNTER,
  };

  int ret = irq_read_data(&fam);

  if (fam.metric.num > 0) {
    int status = plugin_dispatch_metric_family(&fam);
    if (status != 0) {
      ERROR("irq plugin: plugin_dispatch_metric_family failed: %s", STRERROR(status));
      ret = -1;
    }
    metric_family_metric_reset(&fam);
  }

  return ret;
}

void module_register(void)
{
  plugin_register_config("irq", irq_config, config_keys, config_keys_num);
  plugin_register_read("irq", irq_read);
}
