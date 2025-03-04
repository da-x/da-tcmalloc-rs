// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2005, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// ---
// Author: Sanjay Ghemawat
//
// TODO: Log large allocations

#include <config.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <inttypes.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>    // for open()
#endif
#ifdef HAVE_MMAP
#include <sys/mman.h>
#endif
#include <errno.h>
#include <assert.h>
#include <sys/types.h>
#include <signal.h>

#include <algorithm>
#include <memory>
#include <string>

#include <gperftools/heap-profiler.h>

#include "base/logging.h"
#include "base/basictypes.h"   // for PRId64, among other things
#include "base/googleinit.h"
#include "base/commandlineflags.h"
#include "malloc_hook-inl.h"
#include "tcmalloc_guard.h"
#include <gperftools/malloc_hook.h>
#include <gperftools/malloc_extension.h>
#include "base/spinlock.h"
#include "base/low_level_alloc.h"
#include "base/sysinfo.h"      // for GetUniquePathFromEnv()
#include "heap-profile-table.h"
#include "memory_region_map.h"
#include "mmap_hook.h"

#ifndef	PATH_MAX
#ifdef MAXPATHLEN
#define	PATH_MAX	MAXPATHLEN
#else
#define	PATH_MAX	4096         // seems conservative for max filename len!
#endif
#endif

using std::string;

//----------------------------------------------------------------------
// Flags that control heap-profiling
//
// The thread-safety of the profiler depends on these being immutable
// after main starts, so don't change them.
//----------------------------------------------------------------------

#undef EnvToInt64
#define EnvToInt64(a, b) 0
#undef EnvToBool
#define EnvToBool(a, b) false

// Original:
// DEFINE_int64(heap_profile_allocation_interval,
//             EnvToInt64("HEAP_PROFILE_ALLOCATION_INTERVAL", 1 << 30 /*1GB*/),
//             "If non-zero, dump heap profiling information once every specified number of bytes allocated by the program since the last dump.");
int64_t FLAGS_heap_profile_allocation_interval = EnvToInt64("HEAP_PROFILE_ALLOCATION_INTERVAL", 1LL << 30);  // 1GB

// Original:
// DEFINE_int64(heap_profile_deallocation_interval,
//             EnvToInt64("HEAP_PROFILE_DEALLOCATION_INTERVAL", 0),
//             "If non-zero, dump heap profiling information once every specified number of bytes deallocated by the program since the last dump.");
int64_t FLAGS_heap_profile_deallocation_interval = EnvToInt64("HEAP_PROFILE_DEALLOCATION_INTERVAL", 0);

// Original comment:
// We could also add flags that report whenever inuse_bytes changes by X or -X, but there hasn't been a need for that yet, so we haven't.
// Original:
// DEFINE_int64(heap_profile_inuse_interval,
//             EnvToInt64("HEAP_PROFILE_INUSE_INTERVAL", 100 << 20 /*100MB*/),
//             "If non-zero, dump heap profiling information whenever the high-water memory usage mark increases by the specified number of bytes.");
int64_t FLAGS_heap_profile_inuse_interval = EnvToInt64("HEAP_PROFILE_INUSE_INTERVAL", 100LL << 20);  // 100MB

// Original:
// DEFINE_int64(heap_profile_time_interval,
//             EnvToInt64("HEAP_PROFILE_TIME_INTERVAL", 0),
//             "If non-zero, dump heap profiling information once every specified number of seconds since the last dump.");
int64_t FLAGS_heap_profile_time_interval = EnvToInt64("HEAP_PROFILE_TIME_INTERVAL", 0);

// Original:
// DEFINE_bool(mmap_log,
//            EnvToBool("HEAP_PROFILE_MMAP_LOG", false),
//            "Should mmap/munmap calls be logged?");
bool FLAGS_mmap_log = EnvToBool("HEAP_PROFILE_MMAP_LOG", false);

// Original:
// DEFINE_bool(mmap_profile,
//            EnvToBool("HEAP_PROFILE_MMAP", false),
//            "If heap-profiling is on, also profile mmap, mremap, and sbrk)");
bool FLAGS_mmap_profile = EnvToBool("HEAP_PROFILE_MMAP", false);

// Original:
// DEFINE_bool(only_mmap_profile,
//            EnvToBool("HEAP_PROFILE_ONLY_MMAP", false),
//            "If heap-profiling is on, only profile mmap, mremap, and sbrk; do not profile malloc/new/etc");
bool FLAGS_only_mmap_profile = EnvToBool("HEAP_PROFILE_ONLY_MMAP", false);

//----------------------------------------------------------------------
// Locking
//----------------------------------------------------------------------

// A pthread_mutex has way too much lock contention to be used here.
//
// I would like to use Mutex, but it can call malloc(),
// which can cause us to fall into an infinite recursion.
//
// So we use a simple spinlock.
static SpinLock heap_lock;

//----------------------------------------------------------------------
// Simple allocator for heap profiler's internal memory
//----------------------------------------------------------------------

static LowLevelAlloc::Arena *heap_profiler_memory;

static void* ProfilerMalloc(size_t bytes) {
  return LowLevelAlloc::AllocWithArena(bytes, heap_profiler_memory);
}
static void ProfilerFree(void* p) {
  LowLevelAlloc::Free(p);
}

// We use buffers of this size in DoGetHeapProfile.


//----------------------------------------------------------------------
// Profiling control/state data
//----------------------------------------------------------------------

// Access to all of these is protected by heap_lock.
static bool  is_on = false;           // If are on as a subsytem.
static bool  dumping = false;         // Dumping status to prevent recursion
static bool  exact_path = false;      // Use exact path
static char* filename_prefix = NULL;  // Prefix used for profile file names
                                      // (NULL if no need for dumping yet)
static int   dump_count = 0;          // How many dumps so far
static int64_t last_dump_alloc = 0;     // alloc_size when did we last dump
static int64_t last_dump_free = 0;      // free_size when did we last dump
static int64_t high_water_mark = 0;     // In-use-bytes at last high-water dump
static int64_t last_dump_time = 0;      // The time of the last dump

static HeapProfileTable* heap_profile = NULL;  // the heap profile table

//----------------------------------------------------------------------
// Profile generation
//----------------------------------------------------------------------

// Input must be a buffer of size at least 1MB.
static void DoDumpHeapProfileLocked(tcmalloc::GenericWriter* writer) {
  RAW_DCHECK(heap_lock.IsHeld(), "");
  if (is_on) {
    heap_profile->SaveProfile(writer);
  }
}

extern "C" char* GetHeapProfile() {
  tcmalloc::ChunkedWriterConfig config(ProfilerMalloc, ProfilerFree);

  return tcmalloc::WithWriterToStrDup(config, [] (tcmalloc::GenericWriter* writer) {
    SpinLockHolder l(&heap_lock);
    DoDumpHeapProfileLocked(writer);
  });
}

// defined below
static void NewHook(const void* ptr, size_t size);
static void DeleteHook(const void* ptr);

// Helper for HeapProfilerDump.
static void DumpProfileLocked(const char* reason) {
  RAW_DCHECK(heap_lock.IsHeld(), "");
  RAW_DCHECK(is_on, "");
  RAW_DCHECK(!dumping, "");

  if (filename_prefix == NULL) return;  // we do not yet need dumping

  dumping = true;

  // Make file name
  char file_name[1000];
  dump_count++;

  if (exact_path) {
      snprintf(file_name, sizeof(file_name), "%s", filename_prefix);
  } else {
      snprintf(file_name, sizeof(file_name), "%s.%04d%s",
	      filename_prefix, dump_count, HeapProfileTable::kFileExt);
  }

  // Dump the profile
  RAW_VLOG(10, "Dumping heap profile to %s (%s)", file_name, reason);
  // We must use file routines that don't access memory, since we hold
  // a memory lock now.
  RawFD fd = RawOpenForWriting(file_name);
  if (fd == kIllegalRawFD) {
    RAW_LOG(ERROR, "Failed dumping heap profile to %s. Numeric errno is %d", file_name, errno);
    dumping = false;
    return;
  }

  using FileWriter = tcmalloc::RawFDGenericWriter<1 << 20>;
  FileWriter* writer = new (ProfilerMalloc(sizeof(FileWriter))) FileWriter(fd);

  DoDumpHeapProfileLocked(writer);

  // Note: as part of running destructor, it saves whatever stuff we left buffered in the writer
  writer->~FileWriter();
  ProfilerFree(writer);

  RawClose(fd);

  dumping = false;
}

//----------------------------------------------------------------------
// Profile collection
//----------------------------------------------------------------------

// Dump a profile after either an allocation or deallocation, if
// the memory use has changed enough since the last dump.
static void MaybeDumpProfileLocked() {
  if (!dumping) {
    const HeapProfileTable::Stats& total = heap_profile->total();
    const int64_t inuse_bytes = total.alloc_size - total.free_size;
    bool need_to_dump = false;
    char buf[128];

    if (FLAGS_heap_profile_allocation_interval > 0 &&
        total.alloc_size >=
        last_dump_alloc + FLAGS_heap_profile_allocation_interval) {
      snprintf(buf, sizeof(buf), ("%" PRId64 " MB allocated cumulatively, "
                                  "%" PRId64 " MB currently in use"),
               total.alloc_size >> 20, inuse_bytes >> 20);
      need_to_dump = true;
    } else if (FLAGS_heap_profile_deallocation_interval > 0 &&
               total.free_size >=
               last_dump_free + FLAGS_heap_profile_deallocation_interval) {
      snprintf(buf, sizeof(buf), ("%" PRId64 " MB freed cumulatively, "
                                  "%" PRId64 " MB currently in use"),
               total.free_size >> 20, inuse_bytes >> 20);
      need_to_dump = true;
    } else if (FLAGS_heap_profile_inuse_interval > 0 &&
               inuse_bytes >
               high_water_mark + FLAGS_heap_profile_inuse_interval) {
      snprintf(buf, sizeof(buf), "%" PRId64 " MB currently in use",
               inuse_bytes >> 20);
      need_to_dump = true;
    } else if (FLAGS_heap_profile_time_interval > 0 ) {
      int64_t current_time = time(NULL);
      if (current_time - last_dump_time >=
          FLAGS_heap_profile_time_interval) {
        snprintf(buf, sizeof(buf), "%" PRId64 " sec since the last dump",
                 current_time - last_dump_time);
        need_to_dump = true;
        last_dump_time = current_time;
      }
    }
    if (need_to_dump) {
      DumpProfileLocked(buf);

      last_dump_alloc = total.alloc_size;
      last_dump_free = total.free_size;
      if (inuse_bytes > high_water_mark)
        high_water_mark = inuse_bytes;
    }
  }
}

// Record an allocation in the profile.
static void RecordAlloc(const void* ptr, size_t bytes, int skip_count) {
  // Take the stack trace outside the critical section.
  void* stack[HeapProfileTable::kMaxStackDepth];
  int depth = HeapProfileTable::GetCallerStackTrace(skip_count + 1, stack);
  SpinLockHolder l(&heap_lock);
  if (is_on) {
    heap_profile->RecordAlloc(ptr, bytes, depth, stack);
    MaybeDumpProfileLocked();
  }
}

// Record a deallocation in the profile.
static void RecordFree(const void* ptr) {
  SpinLockHolder l(&heap_lock);
  if (is_on) {
    heap_profile->RecordFree(ptr);
    MaybeDumpProfileLocked();
  }
}

//----------------------------------------------------------------------
// Allocation/deallocation hooks for MallocHook
//----------------------------------------------------------------------

// static
void NewHook(const void* ptr, size_t size) {
  if (ptr != NULL) RecordAlloc(ptr, size, 0);
}

// static
void DeleteHook(const void* ptr) {
  if (ptr != NULL) RecordFree(ptr);
}

static tcmalloc::MappingHookSpace mmap_logging_hook_space;

static void LogMappingEvent(const tcmalloc::MappingEvent& evt) {
  if (!FLAGS_mmap_log) {
    return;
  }

  if (evt.file_valid) {
    // We use PRIxPTR not just '%p' to avoid deadlocks
    // in pretty-printing of NULL as "nil".
    // TODO(maxim): instead should use a safe snprintf reimplementation
    RAW_LOG(INFO,
            "mmap(start=0x%" PRIxPTR ", len=%zu, prot=0x%x, flags=0x%x, "
            "fd=%d, offset=0x%llx) = 0x%" PRIxPTR "",
            (uintptr_t) evt.before_address, evt.after_length, evt.prot,
            evt.flags, evt.file_fd, (unsigned long long) evt.file_off,
            (uintptr_t) evt.after_address);
  } else if (evt.after_valid && evt.before_valid) {
    // We use PRIxPTR not just '%p' to avoid deadlocks
    // in pretty-printing of NULL as "nil".
    // TODO(maxim): instead should use a safe snprintf reimplementation
    RAW_LOG(INFO,
            "mremap(old_addr=0x%" PRIxPTR ", old_size=%zu, "
            "new_size=%zu, flags=0x%x, new_addr=0x%" PRIxPTR ") = "
            "0x%" PRIxPTR "",
            (uintptr_t) evt.before_address, evt.before_length, evt.after_length, evt.flags,
            (uintptr_t) evt.after_address, (uintptr_t) evt.after_address);
  } else if (evt.is_sbrk) {
    intptr_t increment;
    uintptr_t result;
    if (evt.after_valid) {
      increment = evt.after_length;
      result = reinterpret_cast<uintptr_t>(evt.after_address) + evt.after_length;
    } else {
      increment = -static_cast<intptr_t>(evt.before_length);
      result = reinterpret_cast<uintptr_t>(evt.before_address);
    }

    RAW_LOG(INFO, "sbrk(inc=%zd) = 0x%" PRIxPTR "",
                  increment, (uintptr_t) result);
  } else if (evt.before_valid) {
    // We use PRIxPTR not just '%p' to avoid deadlocks
    // in pretty-printing of NULL as "nil".
    // TODO(maxim): instead should use a safe snprintf reimplementation
    RAW_LOG(INFO, "munmap(start=0x%" PRIxPTR ", len=%zu)",
                  (uintptr_t) evt.before_address, evt.before_length);
  }
}

//----------------------------------------------------------------------
// Starting/stopping/dumping
//----------------------------------------------------------------------

extern "C" void HeapProfilerStart(const char* prefix) {
  SpinLockHolder l(&heap_lock);

  if (is_on) return;

  is_on = true;

  RAW_VLOG(10, "Starting tracking the heap");

  // This should be done before the hooks are set up, since it should
  // call new, and we want that to be accounted for correctly.
  MallocExtension::Initialize();

  if (FLAGS_only_mmap_profile) {
    FLAGS_mmap_profile = true;
  }

  if (FLAGS_mmap_profile) {
    // Ask MemoryRegionMap to record all mmap, mremap, and sbrk
    // call stack traces of at least size kMaxStackDepth:
    MemoryRegionMap::Init(HeapProfileTable::kMaxStackDepth,
                          /* use_buckets */ true);
  }

  if (FLAGS_mmap_log) {
    // Install our hooks to do the logging:
    tcmalloc::HookMMapEvents(&mmap_logging_hook_space, LogMappingEvent);
  }

  heap_profiler_memory = LowLevelAlloc::NewArena(nullptr);

  heap_profile = new(ProfilerMalloc(sizeof(HeapProfileTable)))
      HeapProfileTable(ProfilerMalloc, ProfilerFree, FLAGS_mmap_profile);

  last_dump_alloc = 0;
  last_dump_free = 0;
  high_water_mark = 0;
  last_dump_time = 0;

  // We do not reset dump_count so if the user does a sequence of
  // HeapProfilerStart/HeapProfileStop, we will get a continuous
  // sequence of profiles.

  if (FLAGS_only_mmap_profile == false) {
    // Now set the hooks that capture new/delete and malloc/free.
    RAW_CHECK(MallocHook::AddNewHook(&NewHook), "");
    RAW_CHECK(MallocHook::AddDeleteHook(&DeleteHook), "");
  }

  // Copy filename prefix
  RAW_DCHECK(filename_prefix == NULL, "");
  const int prefix_length = strlen(prefix);
  filename_prefix = reinterpret_cast<char*>(ProfilerMalloc(prefix_length + 1));
  memcpy(filename_prefix, prefix, prefix_length);
  filename_prefix[prefix_length] = '\0';
}

extern "C" int IsHeapProfilerRunning() {
  SpinLockHolder l(&heap_lock);
  return is_on ? 1 : 0;   // return an int, because C code doesn't have bool
}

extern "C" void HeapProfilerStop() {
  SpinLockHolder l(&heap_lock);

  if (!is_on) return;

  if (FLAGS_only_mmap_profile == false) {
    // Unset our new/delete hooks, checking they were set:
    RAW_CHECK(MallocHook::RemoveNewHook(&NewHook), "");
    RAW_CHECK(MallocHook::RemoveDeleteHook(&DeleteHook), "");
  }
  if (FLAGS_mmap_log) {
    // Restore mmap/sbrk hooks, checking that our hooks were set:
    tcmalloc::UnHookMMapEvents(&mmap_logging_hook_space);
  }

  // free profile
  heap_profile->~HeapProfileTable();
  ProfilerFree(heap_profile);
  heap_profile = NULL;

  // free prefix
  ProfilerFree(filename_prefix);
  filename_prefix = NULL;

  if (!LowLevelAlloc::DeleteArena(heap_profiler_memory)) {
    RAW_LOG(FATAL, "Memory leak in HeapProfiler:");
  }

  if (FLAGS_mmap_profile) {
    MemoryRegionMap::Shutdown();
  }

  is_on = false;
}

/* Set variables */
extern "C" void HeapProfilerSetVars(const struct HeapProfilerVars *vars)
{
    FLAGS_heap_profile_allocation_interval = vars->heap_profile_allocation_interval;
    FLAGS_heap_profile_deallocation_interval = vars->heap_profile_deallocation_interval;
    FLAGS_heap_profile_inuse_interval = vars->heap_profile_inuse_interval;
    FLAGS_heap_profile_time_interval = vars->heap_profile_time_interval;
    FLAGS_mmap_log = vars->mmap_log;
    FLAGS_mmap_profile = vars->mmap_profile;
    FLAGS_only_mmap_profile = vars->only_mmap_profile;
}

/* Disregard 'prefix' and set pathname for next dump */
extern "C" void HeapProfilerSetExactPath(const char *path) {
  SpinLockHolder l(&heap_lock);
  RAW_DCHECK(filename_prefix == NULL, "");
  ProfilerFree(filename_prefix);

  const int prefix_length = strlen(path);
  filename_prefix = reinterpret_cast<char*>(ProfilerMalloc(prefix_length + 1));
  memcpy(filename_prefix, path, prefix_length);
  filename_prefix[prefix_length] = '\0';
  exact_path = true;
}

extern "C" void HeapProfilerDump(const char *reason) {
  SpinLockHolder l(&heap_lock);
  if (is_on && !dumping) {
    DumpProfileLocked(reason);
  }
}

// Signal handler that is registered when a user selectable signal
// number is defined in the environment variable HEAPPROFILESIGNAL.
static void HeapProfilerDumpSignal(int signal_number) {
  (void)signal_number;
  if (!heap_lock.TryLock()) {
    return;
  }
  if (is_on && !dumping) {
    DumpProfileLocked("signal");
  }
  heap_lock.Unlock();
}


//----------------------------------------------------------------------
// Initialization/finalization code
//----------------------------------------------------------------------

// Initialization code
static void HeapProfilerInit() {
  // Everything after this point is for setting up the profiler based on envvar
  char fname[PATH_MAX];
  if (!GetUniquePathFromEnv("HEAPPROFILE", fname)) {
    return;
  }
  // We do a uid check so we don't write out files in a setuid executable.
#ifdef HAVE_GETEUID
  if (getuid() != geteuid()) {
    RAW_LOG(WARNING, ("HeapProfiler: ignoring HEAPPROFILE because "
                      "program seems to be setuid\n"));
    return;
  }
#endif

  char *signal_number_str = getenv("HEAPPROFILESIGNAL");
  if (signal_number_str != NULL) {
    long int signal_number = strtol(signal_number_str, NULL, 10);
    intptr_t old_signal_handler = reinterpret_cast<intptr_t>(signal(signal_number, HeapProfilerDumpSignal));
    if (old_signal_handler == reinterpret_cast<intptr_t>(SIG_ERR)) {
      RAW_LOG(FATAL, "Failed to set signal. Perhaps signal number %s is invalid\n", signal_number_str);
    } else if (old_signal_handler == 0) {
      RAW_LOG(INFO,"Using signal %d as heap profiling switch", signal_number);
    } else {
      RAW_LOG(FATAL, "Signal %d already in use\n", signal_number);
    }
  }

  HeapProfileTable::CleanupOldProfiles(fname);

  HeapProfilerStart(fname);
}

// class used for finalization -- dumps the heap-profile at program exit
struct HeapProfileEndWriter {
  ~HeapProfileEndWriter() {
    char buf[128];
    if (heap_profile) {
      const HeapProfileTable::Stats& total = heap_profile->total();
      const int64_t inuse_bytes = total.alloc_size - total.free_size;

      if ((inuse_bytes >> 20) > 0) {
        snprintf(buf, sizeof(buf), ("Exiting, %" PRId64 " MB in use"),
                 inuse_bytes >> 20);
      } else if ((inuse_bytes >> 10) > 0) {
        snprintf(buf, sizeof(buf), ("Exiting, %" PRId64 " kB in use"),
                 inuse_bytes >> 10);
      } else {
        snprintf(buf, sizeof(buf), ("Exiting, %" PRId64 " bytes in use"),
                 inuse_bytes);
      }
    } else {
      snprintf(buf, sizeof(buf), ("Exiting"));
    }
    HeapProfilerDump(buf);
  }
};

// We want to make sure tcmalloc is up and running before starting the profiler
static const TCMallocGuard tcmalloc_initializer;
static HeapProfileEndWriter heap_profile_end_writer;
