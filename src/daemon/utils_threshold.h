/**
 * collectd - src/utils_threshold.h
 * Copyright (C) 2014       Pierre-Yves Ritschard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *   Pierre-Yves Ritschard <pyr at spootnik.org>
 **/

#ifndef UTILS_THRESHOLD_H
#define UTILS_THRESHOLD_H 1

#define UT_FLAG_INVERT 0x01
#define UT_FLAG_PERSIST 0x02
#define UT_FLAG_INTERESTING 0x04
#define UT_FLAG_PERSIST_OK 0x08

typedef struct threshold_s {
  char *name;
  label_set_t labels;
  gauge_t warning_min;
  gauge_t warning_max;
  gauge_t failure_min;
  gauge_t failure_max;
  gauge_t hysteresis;
  unsigned int flags;
  int hits;
  struct threshold_s *next;
} threshold_t;

extern c_avl_tree_t *threshold_tree;

threshold_t *threshold_get(const char *metric_name);

#endif /* UTILS_THRESHOLD_H */
