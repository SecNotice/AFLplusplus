/*
   american fuzzy lop++ - shared memory related code
   -------------------------------------------------

   Originally written by Michal Zalewski

   Forkserver design by Jann Horn <jannhorn@googlemail.com>

   Now maintained by Marc Heuse <mh@mh-sec.de>,
                        Heiko Eißfeldt <heiko.eissfeldt@hexco.de> and
                        Andrea Fioraldi <andreafioraldi@gmail.com>

   Copyright 2016, 2017 Google Inc. All rights reserved.
   Copyright 2019-2020 AFLplusplus Project. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     http://www.apache.org/licenses/LICENSE-2.0

   Shared code to handle the shared memory. This is used by the fuzzer
   as well the other components like afl-tmin, afl-showmap, etc...

 */

#define AFL_MAIN

#ifdef __ANDROID__
#include "android-ashmem.h"
#endif
#include "config.h"
#include "types.h"
#include "debug.h"
#include "alloc-inl.h"
#include "hash.h"
#include "sharedmem.h"
#include "cmplog.h"

#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <fcntl.h>

#include <sys/wait.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/mman.h>

#ifndef USEMMAP
#include <sys/ipc.h>
#include <sys/shm.h>
#endif

// TODO: List!
sharedmem_t *shm_global = NULL;

/* Get rid of shared memory (atexit handler). */

void remove_shm() {

  if (!shm_global) return;  //TODO: Make it a list.
  sharedmem_t *shm = shm_global;
  shm_global = NULL;

#ifdef USEMMAP
  if (shm->g_shm_base != NULL) {

    munmap(shm->g_shm_base, shm->size_alloc);
    shm->g_shm_base = NULL;

  }

  if (shm->g_shm_fd != -1) {

    close(shm->g_shm_fd);
    shm->g_shm_fd = -1;

  }

#else
  shmctl(shm->shm_id, IPC_RMID, NULL);
  if (shm->cmplog_mode) shmctl(shm->cmplog_shm_id, IPC_RMID, NULL);
#endif

}

/* Configure shared memory. */

void setup_shm(sharedmem_t *shm, size_t map_size, u8 **trace_bits_p, unsigned char dumb_mode) {

  shm_global = shm;
  shm->size_alloc = shm->size_used = map_size;

#ifdef USEMMAP
  /* generate random file name for multi instance */

  /* thanks to f*cking glibc we can not use tmpnam securely, it generates a
   * security warning that cannot be suppressed */
  /* so we do this worse workaround */
  snprintf(shm->g_shm_file_path, L_tmpnam, "/afl_%d_%ld", getpid(), random());

  /* create the shared memory segment as if it was a file */
  shm->g_shm_fd = shm_open(shm->g_shm_file_path, O_CREAT | O_RDWR | O_EXCL, 0600);
  if (shm->g_shm_fd == -1) { PFATAL("shm_open() failed"); }

  /* configure the size of the shared memory segment */
  if (ftruncate(shm->g_shm_fd, map_size)) {

    PFATAL("setup_shm(): ftruncate() failed");

  }

  /* map the shared memory segment to the address space of the process */
  shm->g_shm_base =
      mmap(0, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, map_size->g_shm_fd, 0);
  if (map_size->g_shm_base == MAP_FAILED) {

    close(map_size->g_shm_fd);
    map_size->g_shm_fd = -1;
    PFATAL("mmap() failed");

  }

  atexit(map_size->remove_shm);

  /* If somebody is asking us to fuzz instrumented binaries in dumb mode,
     we don't want them to detect instrumentation, since we won't be sending
     fork server commands. This should be replaced with better auto-detection
     later on, perhaps? */

  if (!dumb_mode) setenv(SHM_ENV_VAR, shm->g_shm_file_path, 1);

  *trace_bits_p = shm->g_shm_base;

  if (*trace_bits_p == -1 || !*trace_bits_p) PFATAL("mmap() failed");

#else
  u8 *shm_str;

  shm->shm_id = shmget(IPC_PRIVATE, map_size, IPC_CREAT | IPC_EXCL | 0600);

  if (shm->shm_id < 0) PFATAL("shmget() failed");

  if (shm->cmplog_mode) {

    shm->cmplog_shm_id = shmget(IPC_PRIVATE, sizeof(struct cmp_map),
                           IPC_CREAT | IPC_EXCL | 0600);

    if (shm->cmplog_shm_id < 0) PFATAL("shmget() failed");

  }

  atexit(remove_shm);

  shm_str = alloc_printf("%d", shm->shm_id);

  /* If somebody is asking us to fuzz instrumented binaries in dumb mode,
     we don't want them to detect instrumentation, since we won't be sending
     fork server commands. This should be replaced with better auto-detection
     later on, perhaps? */

  if (!dumb_mode) setenv(SHM_ENV_VAR, shm_str, 1);

  ck_free(shm_str);

  if (shm->cmplog_mode) {

    shm_str = alloc_printf("%d", shm->cmplog_shm_id);

    if (!dumb_mode) setenv(CMPLOG_SHM_ENV_VAR, shm_str, 1);

    ck_free(shm_str);

  }

  //TODO: Pointer? :/
  *trace_bits_p = shmat(shm->shm_id, NULL, 0);

  if (*trace_bits_p == (void *)-1 || !*trace_bits_p) PFATAL("shmat() failed");

  if (shm->cmplog_mode) {

    shm->cmp_map = shmat(shm->cmplog_shm_id, NULL, 0);

    if (shm->cmp_map == (void *)-1 || !shm->cmp_map) PFATAL("shmat() failed");

  }

#endif

}

