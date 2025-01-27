/**
 * collectd - src/cpusleep.c
 * Copyright (C) 2016 rinigus
 *
 * The MIT License (MIT)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *	 rinigus <http://github.com/rinigus>
 *
 * CPU sleep is reported in milliseconds of sleep per second of wall
 * time. For that, the time difference between BOOT and MONOTONIC clocks
 * is reported using derive type.
 **/

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"
#include <time.h>

static void cpusleep_submit(counter_t cpu_sleep)
{
  metric_family_t fam = {
    .name = "host_cpusleep_milliseconds_total",
    .type = METRIC_TYPE_COUNTER,
  };

  metric_family_metric_append(&fam, (metric_t){ .value.counter = cpu_sleep, });

  int status = plugin_dispatch_metric_family(&fam);
  if (status != 0)
    ERROR("cpusleep plugin: plugin_dispatch_metric_family failed: %s", STRERROR(status));

  metric_family_metric_reset(&fam);
}

static int cpusleep_read(void)
{
  struct timespec b, m;
  if (clock_gettime(CLOCK_BOOTTIME, &b) < 0) {
    ERROR("cpusleep plugin: clock_boottime failed");
    return -1;
  }

  if (clock_gettime(CLOCK_MONOTONIC, &m) < 0) {
    ERROR("cpusleep plugin: clock_monotonic failed");
    return -1;
  }

  // to avoid false positives in counter overflow due to reboot,
  // derive is used. Sleep is calculated in milliseconds
  derive_t diffsec = b.tv_sec - m.tv_sec;
  derive_t diffnsec = b.tv_nsec - m.tv_nsec;
  derive_t sleep = diffsec * 1000 + diffnsec / 1000000;

  cpusleep_submit(sleep);

  return 0;
}

void module_register(void)
{
  plugin_register_read("cpusleep", cpusleep_read);
}
