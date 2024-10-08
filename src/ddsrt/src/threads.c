// Copyright(c) 2006 to 2019 ZettaScale Technology and others
//
// This program and the accompanying materials are made available under the
// terms of the Eclipse Public License v. 2.0 which is available at
// http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
// v. 1.0 which is available at
// http://www.eclipse.org/org/documents/edl-v10.php.
//
// SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause

#include <assert.h>

#include "dds/ddsrt/threads.h"
#include "dds/ddsrt/heap.h"

void
ddsrt_threadattr_init (
  ddsrt_threadattr_t *attr)
{
  assert(attr != NULL);
  attr->schedClass = DDSRT_SCHED_DEFAULT;
  attr->schedPriority = 0;
  attr->schedAffinityN = 0;
  attr->schedAffinitySet = NULL;
  attr->stackSize = 0;
}
