/**
 * collectd - src/load.c
 * Copyright (C) 2005-2008  Florian octo Forster
 * Copyright (C) 2009       Manuel Sanmartin
 * Copyright (C) 2013       Vedran Bartonicek
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
 *   Florian octo Forster <octo at collectd.org>
 *   Manuel Sanmartin
 *   Vedran Bartonicek <vbartoni at gmail.com>
 **/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"

#include <unistd.h>

#ifdef HAVE_SYS_LOADAVG_H
#include <sys/loadavg.h>
#endif

#if HAVE_STATGRAB_H
#include <statgrab.h>
#endif

#ifdef HAVE_GETLOADAVG
#if !defined(LOADAVG_1MIN) || !defined(LOADAVG_5MIN) || !defined(LOADAVG_15MIN)
#define LOADAVG_1MIN 0
#define LOADAVG_5MIN 1
#define LOADAVG_15MIN 2
#endif
#endif /* defined(HAVE_GETLOADAVG) */

#ifdef HAVE_PERFSTAT
#include <libperfstat.h>
#include <sys/proc.h> /* AIX 5 */
#include <sys/protosw.h>
#endif /* HAVE_PERFSTAT */

enum {
  FAM_LOAD_1MIN = 0,
  FAM_LOAD_5MIN,
  FAM_LOAD_15MIN,
  FAM_LOAD_MAX
};

static void load_submit(gauge_t snum, gauge_t mnum, gauge_t lnum)
{
  metric_family_t fams[FAM_LOAD_MAX] = {
    [FAM_LOAD_1MIN] = {
      .name = "host_load1",
      .type = METRIC_TYPE_GAUGE,
      .help = "1m load average",
    },
    [FAM_LOAD_5MIN] = {
      .name = "host_load5",
      .type = METRIC_TYPE_GAUGE,
      .help = "5m load average",
    },
    [FAM_LOAD_15MIN] = {
      .name = "host_load15",
      .type = METRIC_TYPE_GAUGE,
      .help = "15m load average",
    },
  };

  metric_family_metric_append(&fams[FAM_LOAD_1MIN], (metric_t){.value.gauge = snum,});
  metric_family_metric_append(&fams[FAM_LOAD_5MIN], (metric_t){.value.gauge = mnum,});
  metric_family_metric_append(&fams[FAM_LOAD_15MIN], (metric_t){.value.gauge = lnum,});

  for (size_t i=0; i < FAM_LOAD_MAX; i++) {
    int status = plugin_dispatch_metric_family(&fams[i]);
    if (status != 0) {
      ERROR("load plugin: plugin_dispatch_metric_family failed: %s",
          STRERROR(status));
    }
    metric_family_metric_reset(&fams[i]);
  }
}

static int load_read(void)
{
#if defined(HAVE_GETLOADAVG)
  double load[3];

  if (getloadavg(load, 3) == 3)
    load_submit(load[LOADAVG_1MIN], load[LOADAVG_5MIN], load[LOADAVG_15MIN]);
  else {
    WARNING("load: getloadavg failed: %s", STRERRNO);
  }
  /* #endif HAVE_GETLOADAVG */

#elif defined(KERNEL_LINUX)
  gauge_t snum, mnum, lnum;
  FILE *loadavg;
  char buffer[16];

  char *fields[8];
  int numfields;

  if ((loadavg = fopen("/proc/loadavg", "r")) == NULL) {
    WARNING("load: fopen: %s", STRERRNO);
    return -1;
  }

  if (fgets(buffer, 16, loadavg) == NULL) {
    WARNING("load: fgets: %s", STRERRNO);
    fclose(loadavg);
    return -1;
  }

  if (fclose(loadavg)) {
    WARNING("load: fclose: %s", STRERRNO);
  }

  numfields = strsplit(buffer, fields, 8);

  if (numfields < 3)
    return -1;

  snum = atof(fields[0]);
  mnum = atof(fields[1]);
  lnum = atof(fields[2]);

  load_submit(snum, mnum, lnum);

/* #endif KERNEL_LINUX */
#elif HAVE_LIBSTATGRAB
  gauge_t snum, mnum, lnum;
  sg_load_stats *ls;

  if ((ls = sg_get_load_stats()) == NULL)
    return;

  snum = ls->min1;
  mnum = ls->min5;
  lnum = ls->min15;

  load_submit(snum, mnum, lnum);

/* #endif HAVE_LIBSTATGRAB */
#elif HAVE_PERFSTAT
  gauge_t snum, mnum, lnum;
  perfstat_cpu_total_t cputotal;

  if (perfstat_cpu_total(NULL, &cputotal, sizeof(perfstat_cpu_total_t), 1) <
      0) {
    WARNING("load: perfstat_cpu : %s", STRERRNO);
    return -1;
  }

  snum = (float)cputotal.loadavg[0] / (float)(1 << SBITS);
  mnum = (float)cputotal.loadavg[1] / (float)(1 << SBITS);
  lnum = (float)cputotal.loadavg[2] / (float)(1 << SBITS);

  load_submit(snum, mnum, lnum);

/* #endif HAVE_PERFSTAT */
#else
#error "No applicable input method."
#endif

  return 0;
}

void module_register(void)
{
  plugin_register_read("load", load_read);
}
