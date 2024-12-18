/*
   american fuzzy lop++ - bitmap related routines
   ----------------------------------------------

   Originally written by Michal Zalewski

   Now maintained by Marc Heuse <mh@mh-sec.de>,
                        Heiko Eißfeldt <heiko.eissfeldt@hexco.de> and
                        Andrea Fioraldi <andreafioraldi@gmail.com>

   Copyright 2016, 2017 Google Inc. All rights reserved.
   Copyright 2019-2023 AFLplusplus Project. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     https://www.apache.org/licenses/LICENSE-2.0

   This is the real deal: the program takes an instrumented binary and
   attempts a variety of basic fuzzing tricks, paying close attention to
   how they affect the execution path.

 */

#include "afl-fuzz.h"
#include "debug.h"
#include "aflrun.h"
#include <limits.h>
#if !defined NAME_MAX
  #define NAME_MAX _XOPEN_NAME_MAX
#endif

/* Write bitmap to file. The bitmap is useful mostly for the secret
   -B option, to focus a separate fuzzing session on a particular
   interesting input without rediscovering all the others. */

void write_bitmap(afl_state_t *afl) {

  u8  fname[PATH_MAX];
  s32 fd;

  if (!afl->bitmap_changed) { return; }
  afl->bitmap_changed = 0;

  snprintf(fname, PATH_MAX, "%s/fuzz_bitmap", afl->out_dir);
  fd = open(fname, O_WRONLY | O_CREAT | O_TRUNC, DEFAULT_PERMISSION);

  if (fd < 0) { PFATAL("Unable to open '%s'", fname); }

  ck_write(fd, afl->virgin_bits, afl->fsrv.map_size, fname);

  close(fd);

}

/* Count the number of bits set in the provided bitmap. Used for the status
   screen several times every second, does not have to be fast. */

u32 count_bits(afl_state_t *afl, u8 *mem) {

  u32 *ptr = (u32 *)mem;
  u32  i = ((afl->fsrv.real_map_size + 3) >> 2);
  u32  ret = 0;

  while (i--) {

    u32 v = *(ptr++);

    /* This gets called on the inverse, virgin bitmap; optimize for sparse
       data. */

    if (likely(v == 0xffffffff)) {

      ret += 32;
      continue;

    }

    v -= ((v >> 1) & 0x55555555);
    v = (v & 0x33333333) + ((v >> 2) & 0x33333333);
    ret += (((v + (v >> 4)) & 0xF0F0F0F) * 0x01010101) >> 24;

  }

  return ret;

}

/* Count the number of bytes set in the bitmap. Called fairly sporadically,
   mostly to update the status screen or calibrate and examine confirmed
   new paths. */

u32 count_bytes(afl_state_t *afl, u8 *mem) {

  u32 *ptr = (u32 *)mem;
  u32  i = ((afl->fsrv.real_map_size + 3) >> 2);
  u32  ret = 0;

  while (i--) {

    u32 v = *(ptr++);

    if (likely(!v)) { continue; }
    if (v & 0x000000ffU) { ++ret; }
    if (v & 0x0000ff00U) { ++ret; }
    if (v & 0x00ff0000U) { ++ret; }
    if (v & 0xff000000U) { ++ret; }

  }

  return ret;

}

/* Count the number of non-255 bytes set in the bitmap. Used strictly for the
   status screen, several calls per second or so. */

u32 count_non_255_bytes(afl_state_t *afl, u8 *mem) {

  u32 *ptr = (u32 *)mem;
  u32  i = ((afl->fsrv.real_map_size + 3) >> 2);
  u32  ret = 0;

  while (i--) {

    u32 v = *(ptr++);

    /* This is called on the virgin bitmap, so optimize for the most likely
       case. */

    if (likely(v == 0xffffffffU)) { continue; }
    if ((v & 0x000000ffU) != 0x000000ffU) { ++ret; }
    if ((v & 0x0000ff00U) != 0x0000ff00U) { ++ret; }
    if ((v & 0x00ff0000U) != 0x00ff0000U) { ++ret; }
    if ((v & 0xff000000U) != 0xff000000U) { ++ret; }

  }

  return ret;

}

/* Destructively simplify trace by eliminating hit count information
   and replacing it with 0x80 or 0x01 depending on whether the tuple
   is hit or not. Called on every new crash or timeout, should be
   reasonably fast. */
const u8 simplify_lookup[256] = {

    [0] = 1, [1 ... 255] = 128

};

/* Destructively classify execution counts in a trace. This is used as a
   preprocessing step for any newly acquired traces. Called on every exec,
   must be fast. */

const u8 count_class_lookup8[256] = {

    [0] = 0,
    [1] = 1,
    [2] = 2,
    [3] = 4,
    [4 ... 7] = 8,
    [8 ... 15] = 16,
    [16 ... 31] = 32,
    [32 ... 127] = 64,
    [128 ... 255] = 128

};

u16 count_class_lookup16[65536];

void init_count_class16(void) {

  u32 b1, b2;

  for (b1 = 0; b1 < 256; b1++) {

    for (b2 = 0; b2 < 256; b2++) {

      count_class_lookup16[(b1 << 8) + b2] =
          (count_class_lookup8[b1] << 8) | count_class_lookup8[b2];

    }

  }

}

/* Import coverage processing routines. */

#ifdef WORD_SIZE_64
  #include "coverage-64.h"
#else
  #include "coverage-32.h"
#endif

/* Check if the current execution path brings anything new to the table.
   Update virgin bits to reflect the finds. Returns 1 if the only change is
   the hit-count for a particular tuple; 2 if there are new tuples seen.
   Updates the map, so subsequent calls will always return 0.

   This function is called after every exec() on a fairly large buffer, so
   it needs to be fast. We do this in 32-bit and 64-bit flavors. */

inline u8 has_new_bits(afl_state_t *afl, u8 *virgin_map) {

#ifdef WORD_SIZE_64

  u64 *current = (u64 *)afl->fsrv.trace_bits;
  u64 *virgin = (u64 *)virgin_map;

  u32 i = ((afl->fsrv.real_map_size + 7) >> 3);

#else

  u32 *current = (u32 *)afl->fsrv.trace_bits;
  u32 *virgin = (u32 *)virgin_map;

  u32 i = ((afl->fsrv.real_map_size + 3) >> 2);

#endif                                                     /* ^WORD_SIZE_64 */

  u8 ret = 0;
  while (i--) {

    if (unlikely(*current)) discover_word(&ret, current, virgin);

    current++;
    virgin++;

  }

  if (unlikely(ret) && likely(virgin_map == afl->virgin_bits))
    afl->bitmap_changed = 1;

  return ret;

}

inline u8 has_new_bits_mul(afl_state_t *afl,
  u8* const *virgin_maps, u8** p_new_bits, size_t num, u8 modify) {

  u8* new_bits = *p_new_bits = afl_realloc((void **)p_new_bits, sizeof(u8) * num);
  memset(new_bits, 0, sizeof(u8) * num);

  // TODO: 32-bit
  u64 *current = (u64 *)afl->fsrv.trace_bits;
  u64* const *virgins = (u64* const *)virgin_maps;

  u32 len = ((afl->fsrv.real_map_size + 7) >> 3);

  for (u32 i = 0; i < len; ++i, ++current) {

    if (unlikely(*current))
      discover_word_mul(new_bits, current, virgins, num, i, modify);

  }

  u8 primary = new_bits[0], diversity = 0;
  for (size_t i = 1; i < num; ++i) // Get max level of new edge from all div maps
    diversity = MAX(diversity, new_bits[i]);

  // lowest 2 bits are result from primary map,
  // and 2-3 bits are from diversity maps
  return primary | (diversity << 2);

}

/* A combination of classify_counts and has_new_bits. If 0 is returned, then the
 * trace bits are kept as-is. Otherwise, the trace bits are overwritten with
 * classified values.
 *
 * This accelerates the processing: in most cases, no interesting behavior
 * happen, and the trace bits will be discarded soon. This function optimizes
 * for such cases: one-pass scan on trace bits without modifying anything. Only
 * on rare cases it fall backs to the slow path: classify_counts() first, then
 * return has_new_bits(). */

static u8 has_new_bits_unclassified(
  afl_state_t *afl, u8 *const * virgin_maps, size_t num) {

  /* Handle the hot path first: no new coverage */
  u8 *end = afl->fsrv.trace_bits + afl->fsrv.map_size;

#ifdef WORD_SIZE_64

  if (!skim((const u64* const*)virgin_maps, num,
    (u64 *)afl->fsrv.trace_bits, (u64 *)end))
    return 0;

#else

  #error "TODO: 32-bit"
  if (!skim((u32 *)virgin_map, (u32 *)afl->fsrv.trace_bits, (u32 *)end))
    return 0;

#endif                                                     /* ^WORD_SIZE_64 */

  // We don't classify here and call `has_new_bits_mul` here,
  // we because some virgin maps may be missed due to incomplete fringe
  return 1;
}

/* Compact trace bytes into a smaller bitmap. We effectively just drop the
   count information here. This is called only sporadically, for some
   new paths. */

void minimize_bits(afl_state_t *afl, u8 *dst, u8 *src) {

  u32 i = 0;

  while (i < afl->fsrv.map_size) {

    if (*(src++)) { dst[i >> 3] |= 1 << (i & 7); }
    ++i;

  }

}

#ifndef SIMPLE_FILES

/* Construct a file name for a new test case, capturing the operation
   that led to its discovery. Returns a ptr to afl->describe_op_buf_256. */

u8 *describe_op(afl_state_t *afl,
  u8 new_bits, u8 new_paths, size_t max_description_len) {

  u8 is_timeout = 0;

  if (new_bits & 0xf0) {

    new_bits -= 0x80;
    is_timeout = 1;

  }

  u8 new_div = new_bits >> 2;
  new_bits &= 3;

  size_t real_max_len =
      MIN(max_description_len, sizeof(afl->describe_op_buf_256));
  u8 *ret = afl->describe_op_buf_256;

  if (unlikely(afl->syncing_party)) {

    sprintf(ret, "sync:%s,src:%06u", afl->syncing_party, afl->syncing_case);

  } else {

    sprintf(ret, "src:%06u", afl->current_entry);

    if (afl->splicing_with >= 0) {

      sprintf(ret + strlen(ret), "+%06d", afl->splicing_with);

    }

    sprintf(ret + strlen(ret), ",time:%llu,execs:%llu",
            get_cur_time() + afl->prev_run_time - afl->start_time,
            afl->fsrv.total_execs);

    if (afl->current_custom_fuzz &&
        afl->current_custom_fuzz->afl_custom_describe) {

      /* We are currently in a custom mutator that supports afl_custom_describe,
       * use it! */

      size_t len_current = strlen(ret);
      ret[len_current++] = ',';
      ret[len_current] = '\0';

      ssize_t size_left = real_max_len - len_current -
        strlen(",+cov2") - strlen(",+div2") - strlen(",+path") - 2;
      if (is_timeout) { size_left -= strlen(",+tout"); }
      if (unlikely(size_left <= 0)) FATAL("filename got too long");

      const char *custom_description =
          afl->current_custom_fuzz->afl_custom_describe(
              afl->current_custom_fuzz->data, size_left);
      if (!custom_description || !custom_description[0]) {

        DEBUGF("Error getting a description from afl_custom_describe");
        /* Take the stage name as description fallback */
        sprintf(ret + len_current, "op:%s", afl->stage_short);

      } else {

        /* We got a proper custom description, use it */
        strncat(ret + len_current, custom_description, size_left);

      }

    } else {

      /* Normal testcase descriptions start here */
      sprintf(ret + strlen(ret), ",op:%s", afl->stage_short);

      if (afl->stage_cur_byte >= 0) {

        sprintf(ret + strlen(ret), ",pos:%d", afl->stage_cur_byte);

        if (afl->stage_val_type != STAGE_VAL_NONE) {

          sprintf(ret + strlen(ret), ",val:%s%+d",
                  (afl->stage_val_type == STAGE_VAL_BE) ? "be:" : "",
                  afl->stage_cur_val);

        }

      } else {

        sprintf(ret + strlen(ret), ",rep:%d", afl->stage_cur_val);

      }

    }

  }

  if (is_timeout) { strcat(ret, ",+tout"); }

  if (new_bits) {
    strcat(ret, ",+cov");
    if (new_bits == 2) { strcat(ret, "2"); }
  }
  if (new_div) {
    strcat(ret, ",+div");
    if (new_div == 2) { strcat(ret, "2"); }
  }
  if (new_paths) { strcat(ret, ",+path"); }

  if (unlikely(strlen(ret) >= max_description_len))
    FATAL("describe string is too long");

  return ret;

}

#endif                                                     /* !SIMPLE_FILES */

/* Write a message accompanying the crash directory :-) */

void write_crash_readme(afl_state_t *afl) {

  u8    fn[PATH_MAX];
  s32   fd;
  FILE *f;

  u8 val_buf[STRINGIFY_VAL_SIZE_MAX];

  sprintf(fn, "%s/crashes/README.txt", afl->out_dir);

  fd = open(fn, O_WRONLY | O_CREAT | O_EXCL, DEFAULT_PERMISSION);

  /* Do not die on errors here - that would be impolite. */

  if (unlikely(fd < 0)) { return; }

  f = fdopen(fd, "w");

  if (unlikely(!f)) {

    close(fd);
    return;

  }

  fprintf(
      f,
      "Command line used to find this crash:\n\n"

      "%s\n\n"

      "If you can't reproduce a bug outside of afl-fuzz, be sure to set the "
      "same\n"
      "memory limit. The limit used for this fuzzing session was %s.\n\n"

      "Need a tool to minimize test cases before investigating the crashes or "
      "sending\n"
      "them to a vendor? Check out the afl-tmin that comes with the fuzzer!\n\n"

      "Found any cool bugs in open-source tools using afl-fuzz? If yes, please "
      "post\n"
      "to https://github.com/AFLplusplus/AFLplusplus/issues/286 once the "
      "issues\n"
      " are fixed :)\n\n",

      afl->orig_cmdline,
      stringify_mem_size(val_buf, sizeof(val_buf),
                         afl->fsrv.mem_limit << 20));      /* ignore errors */

  fclose(f);

}

/* Check if the result of an execve() during routine fuzzing is interesting,
   save or queue the input test case for further analysis if so. Returns 1 if
   entry is saved, 0 otherwise. */

u8 __attribute__((hot))
save_if_interesting(afl_state_t *afl, void *mem, u32 len, u8 fault, u8 inc) {

  if (unlikely(len == 0)) {
    aflrun_recover_virgin(afl);
    return 0;
  }

  u8  fn[PATH_MAX];
  u8 *queue_fn = "";
  u8  new_bits = 0, new_paths = 0,
    keeping = 0, res, classified = 0, is_timeout = 0;
  s32 fd;
  u64 cksum = 0;

  /* Update path frequency. */

  /* Generating a hash on every input is super expensive. Bad idea and should
     only be used for special schedules */
  if (unlikely(!afl->is_aflrun &&
    afl->schedule >= FAST && afl->schedule <= RARE)) {

    cksum = hash64(afl->fsrv.trace_bits, afl->fsrv.map_size, HASH_CONST);

    /* Saturated increment */
    if (afl->n_fuzz[cksum % N_FUZZ_SIZE] < 0xFFFFFFFF)
      afl->n_fuzz[cksum % N_FUZZ_SIZE]++;

  }

  u8 is_unique = 0;
  if (fault == FSRV_RUN_CRASH || fault == FSRV_RUN_OK) {
    u8 is_crash = fault == FSRV_RUN_CRASH;
    is_unique = get_valuation(afl, afl->argv, mem, len, is_crash);
    if (is_unique) {
      u8 *dop = describe_op(afl, 0, 0, NAME_MAX - strlen("neg_000000_"));
      snprintf(fn, PATH_MAX, "%s/memory/input/%s_%06llu_%s", afl->out_dir,
               is_crash ? "neg" : "pos",
               is_crash ? afl->total_saved_crashes : afl->total_saved_positives,
               dop);

      PAC_LOGF(afl->pacfix_log, "[valuation] [uniq] [val memory/%s/id:%06llu] [file memory/input/%s_%06llu_%s] [time %llu]\n",
        is_crash ? "neg" : "pos", is_crash ? afl->total_saved_crashes : afl->total_saved_positives,
        is_crash ? "neg" : "pos", is_crash ? afl->total_saved_crashes : afl->total_saved_positives,
        dop, get_cur_time() - afl->start_time);

      fd = open(fn, O_WRONLY | O_CREAT | O_EXCL, DEFAULT_PERMISSION);
      if (unlikely(fd < 0)) { PFATAL("Unable to create '%s'", fn); }
      ck_write(fd, mem, len, fn);
      close(fd);
    }
  }

  if (likely(fault == FSRV_RUN_CRASH || fault == FSRV_RUN_OK)) { // fault == afl->crash_mode

    /* Keep only if there are new bits in the map, add to queue for
       future fuzzing, etc. */

    size_t n = afl->fsrv.trace_targets->num;
    afl->virgins = afl_realloc((void **)&afl->virgins, sizeof(u8 *) * (n + 1));
    afl->clusters = afl_realloc((void **)&afl->clusters, sizeof(size_t) * (n+1));
    afl->virgins[0] = afl->virgin_bits;
    afl->clusters[0] = 0; // primary map is always cluster 0
    afl->num_maps = aflrun_get_virgins(afl->fsrv.trace_targets->trace, n,
      afl->virgins + 1, afl->clusters + 1) + 1;
    new_bits = has_new_bits_unclassified(afl, afl->virgins, afl->num_maps);
    if (new_bits) {
      classify_counts(&afl->fsrv);
      classified = 1;
      has_new_bits_mul(afl, afl->virgins, &afl->new_bits, afl->num_maps, 0);
    }
    new_paths = aflrun_has_new_path(afl->fsrv.trace_freachables,
      afl->fsrv.trace_reachables, afl->fsrv.trace_ctx,
      afl->fsrv.trace_virgin->trace, afl->fsrv.trace_virgin->num,
      inc, afl->queued_items,
      new_bits ? afl->new_bits : NULL, afl->clusters, afl->num_maps);

    if (likely(!new_bits && !new_paths)) {

      if (unlikely(afl->crash_mode)) { ++afl->total_crashes; }
      return 0;

    }

    /*/ DEBUG
    ACTF("Number of targets: %d", afl->fsrv.trace_targets->num);
    printf("Targets(Interesting): ");
    for (size_t i = 0; i < n; ++i) {
      printf("%u ", afl->fsrv.trace_targets->trace[i].block);
    }
    printf("\n");
    printf("Clusters(Interesting): ");
    for (size_t i = 0; i < afl->num_maps; ++i) {
      printf("%u ", afl->clusters[i]);
    }
    printf("\n");
    // DEBUG*/

    // We clasify and update bits after related fringes is updated,
    // but before that we may need to update `virgin_maps`
    // because there might be new fringes.

    n = aflrun_max_clusters(afl->queued_items);
    afl->virgins = afl_realloc((void **)&afl->virgins, sizeof(u8*) * n);
    afl->clusters = afl_realloc((void **)&afl->clusters, sizeof(size_t) * n);
    afl->virgins[0] = afl->virgin_bits;
    afl->clusters[0] = 0;
    afl->num_maps = aflrun_get_seed_virgins(
      afl->queued_items, afl->virgins + 1, afl->clusters + 1) + 1;

    if (!classified) {
      classify_counts(&afl->fsrv);
      classified = 1;
    }
    new_bits = has_new_bits_mul(
      afl, afl->virgins, &afl->new_bits, afl->num_maps, 1);

  save_to_queue:

#ifndef SIMPLE_FILES

    queue_fn =
        alloc_printf("%s/queue/id:%06u,%s", afl->out_dir, afl->queued_items,
                     describe_op(afl, new_bits + is_timeout, new_paths,
                                 NAME_MAX - strlen("id:000000,")));

#else

    queue_fn =
        alloc_printf("%s/queue/id_%06u", afl->out_dir, afl->queued_items);

#endif                                                    /* ^!SIMPLE_FILES */
    fd = open(queue_fn, O_WRONLY | O_CREAT | O_EXCL, DEFAULT_PERMISSION);
    if (unlikely(fd < 0)) { PFATAL("Unable to create '%s'", queue_fn); }
    ck_write(fd, mem, len, queue_fn);
    close(fd);
    add_to_queue(afl, queue_fn, len, 0);
    afl->queue_top->tested = 1;
    afl->queue_top->path_cksum = hash64(
      afl->fsrv.trace_ctx, MAP_TR_SIZE(afl->fsrv.num_reachables), HASH_CONST);

    // If the new seed only comes from diversity or path, mark it as extra
    if ((new_bits & 3) == 0 && ((new_bits >> 2) || new_paths)) {
      ++afl->queued_extra;
      afl->queue_top->aflrun_extra = 1;
    }

#ifdef INTROSPECTION
    if (afl->custom_mutators_count && afl->current_custom_fuzz) {

      LIST_FOREACH(&afl->custom_mutator_list, struct custom_mutator, {

        if (afl->current_custom_fuzz == el && el->afl_custom_introspection) {

          const char *ptr = el->afl_custom_introspection(el->data);

          if (ptr != NULL && *ptr != 0) {

            fprintf(afl->introspection_file, "QUEUE CUSTOM %s = %s\n", ptr,
                    afl->queue_top->fname);

          }

        }

      });

    } else if (afl->mutation[0] != 0) {

      fprintf(afl->introspection_file, "QUEUE %s = %s\n", afl->mutation,
              afl->queue_top->fname);

    }

#endif

    if ((new_bits & 3) == 2) {

      afl->queue_top->has_new_cov = 1;
      ++afl->queued_with_cov;

    }

    /* AFLFast schedule? update the new queue entry */
    if (cksum) {

      afl->queue_top->n_fuzz_entry = cksum % N_FUZZ_SIZE;
      afl->n_fuzz[afl->queue_top->n_fuzz_entry] = 1;

    }

    /* due to classify counts we have to recalculate the checksum */
    afl->queue_top->exec_cksum =
        hash64(afl->fsrv.trace_bits, afl->fsrv.map_size, HASH_CONST);

    /* Try to calibrate inline; this also calls update_bitmap_score() when
       successful. */

    res = calibrate_case(afl, afl->queue_top, mem, aflrun_queue_cycle(), 0);

    if (unlikely(res == FSRV_RUN_ERROR)) {

      FATAL("Unable to execute target application");

    }

    if (likely(afl->q_testcase_max_cache_size)) {

      queue_testcase_store_mem(afl, afl->queue_top, mem);

    }

    keeping = 1;

  }
  else {
    aflrun_recover_virgin(afl);
  }

  switch (fault) {

    case FSRV_RUN_TMOUT:

      /* Timeouts are not very interesting, but we're still obliged to keep
         a handful of samples. We use the presence of new bits in the
         hang-specific bitmap as a signal of uniqueness. In "non-instrumented"
         mode, we just keep everything. */

      ++afl->total_tmouts;

      if (afl->saved_hangs >= KEEP_UNIQUE_HANG) { return keeping; }

      if (likely(!afl->non_instrumented_mode)) {

        if (!classified) {

          classify_counts(&afl->fsrv);
          classified = 1;

        }

        simplify_trace(afl, afl->fsrv.trace_bits);

        if (!has_new_bits(afl, afl->virgin_tmout)) { return keeping; }

      }

      is_timeout = 0x80;
#ifdef INTROSPECTION
      if (afl->custom_mutators_count && afl->current_custom_fuzz) {

        LIST_FOREACH(&afl->custom_mutator_list, struct custom_mutator, {

          if (afl->current_custom_fuzz == el && el->afl_custom_introspection) {

            const char *ptr = el->afl_custom_introspection(el->data);

            if (ptr != NULL && *ptr != 0) {

              fprintf(afl->introspection_file,
                      "UNIQUE_TIMEOUT CUSTOM %s = %s\n", ptr,
                      afl->queue_top->fname);

            }

          }

        });

      } else if (afl->mutation[0] != 0) {

        fprintf(afl->introspection_file, "UNIQUE_TIMEOUT %s\n", afl->mutation);

      }

#endif

      /* Before saving, we make sure that it's a genuine hang by re-running
         the target with a more generous timeout (unless the default timeout
         is already generous). */

      if (afl->fsrv.exec_tmout < afl->hang_tmout) {

        u8  new_fault;
        u32 tmp_len = write_to_testcase(afl, &mem, len, 0);

        if (likely(tmp_len)) {

          len = tmp_len;

        } else {

          len = write_to_testcase(afl, &mem, len, 1);

        }

        new_fault = fuzz_run_target(afl, &afl->fsrv, afl->hang_tmout);
        classify_counts(&afl->fsrv);

        /* A corner case that one user reported bumping into: increasing the
           timeout actually uncovers a crash. Make sure we don't discard it if
           so. */

        if (!afl->stop_soon && new_fault == FSRV_RUN_CRASH) {

          goto keep_as_crash;

        }

        if (afl->stop_soon || new_fault != FSRV_RUN_TMOUT) {

          if (afl->afl_env.afl_keep_timeouts) {

            ++afl->saved_tmouts;
            // For saved timeout case, we don't update it with aflrun,
            // so we don't call it with `aflrun_has_new_path`, e.i. `tested = 1`.
            // Also, we need to set virgin map array to have only primary map.
            afl->virgins = afl_realloc((void **)&afl->virgins, sizeof(u8*));
            afl->virgins[0] = afl->virgin_bits;
            afl->clusters[0] = 0;
            afl->num_maps = 1;
            goto save_to_queue;

          } else {

            return keeping;

          }

        }

      }

#ifndef SIMPLE_FILES

      snprintf(fn, PATH_MAX, "%s/hangs/id:%06llu,%s", afl->out_dir,
               afl->saved_hangs,
               describe_op(afl, 0, 0, NAME_MAX - strlen("id:000000,")));

#else

      snprintf(fn, PATH_MAX, "%s/hangs/id_%06llu", afl->out_dir,
               afl->saved_hangs);

#endif                                                    /* ^!SIMPLE_FILES */

      ++afl->saved_hangs;

      afl->last_hang_time = get_cur_time();

      break;

    case FSRV_RUN_CRASH:

    keep_as_crash:

      /* This is handled in a manner roughly similar to timeouts,
         except for slightly different limits and no need to re-run test
         cases. */

      ++afl->total_crashes;

      if (afl->saved_crashes >= KEEP_UNIQUE_CRASH) { return keeping; }

      if (likely(!afl->non_instrumented_mode)) {

        if (!classified) { classify_counts(&afl->fsrv); }

        simplify_trace(afl, afl->fsrv.trace_bits);

        if (!has_new_bits(afl, afl->virgin_crash)) { return keeping; }

      }

      if (unlikely(!afl->saved_crashes) &&
          (afl->afl_env.afl_no_crash_readme != 1)) {

        write_crash_readme(afl);

      }

#ifndef SIMPLE_FILES

      snprintf(fn, PATH_MAX, "%s/crashes/id:%06llu,sig:%02u,%s", afl->out_dir,
               afl->saved_crashes, afl->fsrv.last_kill_signal,
               describe_op(afl, 0, 0, NAME_MAX - strlen("id:000000,sig:00,")));

#else

      snprintf(fn, PATH_MAX, "%s/crashes/id_%06llu_%02u", afl->out_dir,
               afl->saved_crashes, afl->fsrv.last_kill_signal);

#endif                                                    /* ^!SIMPLE_FILES */

      ++afl->saved_crashes;
#ifdef INTROSPECTION
      if (afl->custom_mutators_count && afl->current_custom_fuzz) {

        LIST_FOREACH(&afl->custom_mutator_list, struct custom_mutator, {

          if (afl->current_custom_fuzz == el && el->afl_custom_introspection) {

            const char *ptr = el->afl_custom_introspection(el->data);

            if (ptr != NULL && *ptr != 0) {

              fprintf(afl->introspection_file, "UNIQUE_CRASH CUSTOM %s = %s\n",
                      ptr, afl->queue_top->fname);

            }

          }

        });

      } else if (afl->mutation[0] != 0) {

        fprintf(afl->introspection_file, "UNIQUE_CRASH %s\n", afl->mutation);

      }

#endif
      if (unlikely(afl->infoexec)) {

        // if the user wants to be informed on new crashes - do that
#if !TARGET_OS_IPHONE
        // we dont care if system errors, but we dont want a
        // compiler warning either
        // See
        // https://stackoverflow.com/questions/11888594/ignoring-return-values-in-c
        (void)(system(afl->infoexec) + 1);
#else
        WARNF("command execution unsupported");
#endif

      }

      afl->last_crash_time = get_cur_time();
      afl->last_crash_execs = afl->fsrv.total_execs;

      break;

    case FSRV_RUN_ERROR:
      FATAL("Unable to execute target application");

    default:
      return keeping;

  }

  if (is_unique) { return keeping; } // Already saved

  /* If we're here, we apparently want to save the crash or hang
     test case, too. */

  fd = open(fn, O_WRONLY | O_CREAT | O_EXCL, DEFAULT_PERMISSION);
  if (unlikely(fd < 0)) { PFATAL("Unable to create '%s'", fn); }
  ck_write(fd, mem, len, fn);
  close(fd);

#ifdef __linux__
  if (afl->fsrv.nyx_mode && fault == FSRV_RUN_CRASH) {

    u8 fn_log[PATH_MAX];

    (void)(snprintf(fn_log, PATH_MAX, "%s.log", fn) + 1);
    fd = open(fn_log, O_WRONLY | O_CREAT | O_EXCL, DEFAULT_PERMISSION);
    if (unlikely(fd < 0)) { PFATAL("Unable to create '%s'", fn_log); }

    u32 nyx_aux_string_len = afl->fsrv.nyx_handlers->nyx_get_aux_string(
        afl->fsrv.nyx_runner, afl->fsrv.nyx_aux_string, 0x1000);

    ck_write(fd, afl->fsrv.nyx_aux_string, nyx_aux_string_len, fn_log);
    close(fd);

  }

#endif

  return keeping;

}

// Hashmap implementation
struct hashmap* hashmap_create(u32 table_size) {
  struct hashmap* map = ck_alloc(sizeof(struct hashmap));
  if (map == NULL) {
    printf("Memory allocation failed.\n");
    exit(EXIT_FAILURE);
  }
  map->size = 0;
  map->table_size = table_size;
  map->table = ck_alloc(table_size * sizeof(struct key_value_pair*));
  if (map->table == NULL) {
    printf("Memory allocation failed.\n");
    exit(EXIT_FAILURE);
  }
  for (u32 i = 0; i < table_size; i++) {
    map->table[i] = NULL;
  }
  return map;
}

u32 hashmap_fit(u32 key, u32 table_size) {
  return key % table_size;
}

void hashmap_resize(struct hashmap *map) {

  u32 new_table_size = map->table_size * 2;
  struct key_value_pair **new_table = ck_alloc(new_table_size * sizeof(struct key_value_pair*));
  if (new_table == NULL) {
    printf("Memory allocation failed.\n");
    exit(EXIT_FAILURE);
  }
  for (u32 i = 0; i < map->table_size; i++) {
    struct key_value_pair* pair = map->table[i];
    while (pair != NULL) {
      struct key_value_pair *next = pair->next;
      u32 index = hashmap_fit(pair->key, new_table_size);
      pair->next = new_table[index];
      new_table[index] = pair;
      pair = next;
    }
  }
  ck_free(map->table);
  map->table = new_table;
  map->table_size = new_table_size;

}

u32 hashmap_size(struct hashmap* map) {
  return map->size;
}

// Function to insert a key-value pair into the hash map
void hashmap_insert(struct hashmap* map, u32 key, void* value) {
  u32 index = hashmap_fit(key, map->table_size);
  struct key_value_pair* newPair = ck_alloc(sizeof(struct key_value_pair));
  if (newPair == NULL) {
    printf("Memory allocation failed.\n");
    exit(EXIT_FAILURE);
  }
  newPair->key = key;
  newPair->value = value;
  newPair->next = map->table[index];
  map->table[index] = newPair;
  map->size++;
  if (map->size > map->table_size / 2) {
    hashmap_resize(map);
  }
}

void hashmap_remove(struct hashmap *map, u32 key) {
  u32 index = hashmap_fit(key, map->table_size);
  struct key_value_pair* pair = map->table[index];
  struct key_value_pair* prev = NULL;
  while (pair != NULL) {
    if (pair->key == key) {
      if (!prev) {
        map->table[index] = pair->next;
      } else {
        prev->next = pair->next;
      }
      map->size--;
      ck_free(pair);
      return;
    }
    prev = pair;
    pair = pair->next;
  }
}

struct key_value_pair* hashmap_get(struct hashmap* map, u32 key) {
  u32 index = hashmap_fit(key, map->table_size);
  struct key_value_pair* pair = map->table[index];
  while (pair != NULL) {
    if (pair->key == key) {
      return pair;
    }
    pair = pair->next;
  }
  return NULL;
}

void hashmap_free(struct hashmap* map) {
  for (u32 i = 0; i < map->table_size; i++) {
    struct key_value_pair* pair = map->table[i];
    while (pair != NULL) {
      struct key_value_pair* next = pair->next;
      ck_free(pair);
      pair = next;
    }
  }
  ck_free(map->table);
  ck_free(map);
}

static u32 hash_file(u8 *filename) {

  FILE *file = fopen(filename, "r");

  if (!file) {
    WARNF("Cannot open file %s", filename);
    return 0;
  }

  fseek(file, 0, SEEK_END);
  u64 length = ftell(file);
  fseek(file, 0, SEEK_SET);

  u64 max_read = 1 << 25; // 32MB
  length = length < max_read ? length : max_read;

  u8 *buf = ck_alloc_nozero(length);
  fread(buf, 1, length, file);
  fclose(file);

  u32 hash = hash32(buf, length, HASH_CONST);
  ck_free(buf);
  return hash;

}

/* PacFuzz : We implemented a new function that separated with run_target
   to run the valuation binary. The reason is that we don't want shared memories
   being affected by the valuation binary. So we removed everything related to
   forkserver and shared memories. */

static s32 child_pid = -1;
static volatile u8 child_timed_out = 0;
static u8          kill_signal; /* Signal that killed the child     */
static s32
    dev_urandom_fd = -1,   /* Persistent fd for /dev/urandom   */
    dev_null_fd = -1;      /* Persistent fd for /dev/null      */

static s32 out_dir_fd = -1;  /* FD of the lock file              */

static u8 run_valuation_binary(afl_state_t *afl, char** argv, u32 timeout, char* env_opt) {

  static struct itimerval it;
  static u32 prev_timed_out = 0;
  static u64 exec_ms = 0;

  // Initialize
  if (dev_urandom_fd < 0) {
    dev_urandom_fd = open("/dev/urandom", O_RDONLY);
    if (dev_urandom_fd < 0) PFATAL("[PacFuzz] [run_valuation_binary] Unable to open /dev/urandom");
  }

  if (dev_null_fd < 0) {
    dev_null_fd = open("/dev/null", O_RDWR);
    if (dev_null_fd < 0) PFATAL("[PacFuzz] [run_valuation_binary] Unable to open /dev/null");
  }

  if (out_dir_fd < 0) {
    out_dir_fd = open(afl->out_dir, O_RDONLY);
    if (out_dir_fd < 0) PFATAL("[PacFuzz] [run_valuation_binary] Unable to open out_dir");
  }

  u8 uses_asan = afl->fsrv.uses_asan;

  int status = 0;
  u8 is_run_failed;
  // PAC_LOGF(afl->pacfix_log, "[PacFuzz] [run_valuation_binary] [out_file %s] [out_fd %d]\n", afl->fsrv.out_file ? afl->fsrv.out_file : "", afl->fsrv.out_fd);

  child_timed_out = 0;
  child_pid = fork();
  
  if (child_pid < 0) PFATAL("[PacFuzz] [run_valuation_binary] fork() failed");

    if (!child_pid) {

      struct rlimit r;
      r.rlim_max = r.rlim_cur = 0;

      setrlimit(RLIMIT_CORE, &r); /* Ignore errors */

      /* Isolate the process and configure standard descriptors. If out_file is
         specified, stdin is /dev/null; otherwise, out_fd is cloned instead. */

      setsid();

      dup2(dev_null_fd, 1);
      dup2(dev_null_fd, 2);

      if (!afl->fsrv.use_stdin) {
        dup2(dev_null_fd, 0);
      } else {
        dup2(afl->fsrv.out_fd, 0);
        close(afl->fsrv.out_fd);
      }
      /* On Linux, would be faster to use O_CLOEXEC. Maybe TODO. */

      close(dev_null_fd);
      close(out_dir_fd);
      close(dev_urandom_fd);

      /* Set sane defaults for ASAN if nothing else specified. */
      
      char *envp[] = {
          "ASAN_OPTIONS=abort_on_error=1:halt_on_error=1:detect_leaks=0:"
          "symbolize=0:allocator_may_return_null=1",
          "MSAN_OPTIONS=exit_code=86:halt_on_error=1:symbolize=0:msan_track_"
          "origins=0",
          "UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1:exit_code=54:print_"
          "stacktrace=1",
          env_opt, 0};
      

      execve(argv[0], argv, envp);

      /* Use a distinctive bitmap value to tell the parent about execv()
         falling through. */

      // LOGF("[PacFuzz] [run_valuation_binary] execv() failed\n");
      PAC_LOGF(afl->pacfix_log, "[PacFuzz] [run_valuation_binary] execv() failed\n");
      is_run_failed = 1;
      exit(0);
    }

  /* Configure timeout, as requested by user, then wait for child to terminate. */

  it.it_value.tv_sec = (timeout / 1000);
  it.it_value.tv_usec = (timeout % 1000) * 1000;

  setitimer(ITIMER_REAL, &it, NULL);

  /* The SIGALRM handler simply kills the child_pid and sets child_timed_out. */

  if (waitpid(child_pid, &status, 0) <= 0) PFATAL("[PacFuzz] [run_valuation_binary] waitpid() failed");

  if (!WIFSTOPPED(status)) child_pid = 0;

  getitimer(ITIMER_REAL, &it);
  exec_ms = (u64) timeout - (it.it_value.tv_sec * 1000 +
                             it.it_value.tv_usec / 1000);

  it.it_value.tv_sec = 0;
  it.it_value.tv_usec = 0;

  setitimer(ITIMER_REAL, &it, NULL);

  prev_timed_out = child_timed_out;

  /* Report outcome to caller. */

  if (WIFSIGNALED(status)) {

    kill_signal = WTERMSIG(status);

    if (child_timed_out && kill_signal == SIGKILL) return FAULT_TMOUT;

    return FAULT_CRASH;

  }

  /* A somewhat nasty hack for MSAN, which doesn't support abort_on_error and
     must use a special exit code. */

  if (uses_asan && WEXITSTATUS(status) == MSAN_ERROR) {
    kill_signal = 0;
    return FAULT_CRASH;
  }

  if (is_run_failed)
    return FAULT_ERROR;

  return FAULT_NONE;

}

u8 get_valuation(afl_state_t *afl, char** argv, u8* use_mem, u32 len, u8 crashed) {
  PAC_LOGF(afl->pacfix_log, "[PacFuzz] [targets] [num %d] [time %llu]\n", afl->fsrv.trace_targets->num, get_cur_time() - afl->start_time);
  if (afl->fsrv.trace_targets->num > 0 || (crashed)) {
    u32 val_hash;
    u8 *valuation_file;
    u8 success = run_valuation(afl, crashed, argv, use_mem, len, &val_hash, &valuation_file);
    if (success) {
      save_valuation(afl, val_hash, valuation_file, crashed);
    }
    return success;
  }
  return 0;
}

u8 run_valuation(afl_state_t *afl, u8 crashed, char** argv, void* mem, u32 len, u32 *val_hash, u8 **valuation_file) {
  u8 *valexe = "";
  u8 *covdir = "";
  u8 *tmpfile = "";
  u8 *tmpfile_env = "";
  u8 fault_tmp;
  u8 *tmp_argv1 = "";
  u32 num = 1 + rand_below(afl, ARITH_MAX);

  *val_hash = 0;
  *valuation_file = NULL;

  if(!getenv("PACFIX_VAL_EXE")) return 0;
  if(!getenv("PACFIX_COV_DIR")) return 0;

  valexe = getenv("PACFIX_VAL_EXE");
  covdir = getenv("PACFIX_COV_DIR");

  tmpfile = alloc_printf((crashed ? "%s/__valuation_file_%llu" : "%s/__valuation_file_noncrash_%llu"), covdir, (crashed ? afl->total_saved_crashes : afl->total_saved_positives));
  tmpfile_env = alloc_printf("PACFIX_FILENAME=%s", tmpfile);

  // Remove covdir + "/__tmp_file" (It might not exist, but that's okay)
  chmod(tmpfile,0777);
  u8 error_code = remove(tmpfile);

  (void)write_to_testcase(afl, &mem, len, 0);

  tmp_argv1 = argv[0];
  argv[0] = valexe;
  fault_tmp = run_valuation_binary(afl, argv, 10000, tmpfile_env);
  argv[0] = tmp_argv1;
  ck_free(tmpfile_env);

  // PAC_LOGF(afl->pacfix_log, "[PacFuzz] [run_valuation] [run-completed] [fault %d] [time %llu] [val %s] [file %s]\n", fault_tmp, get_cur_time() - afl->start_time, valexe, tmpfile);

  if (fault_tmp == FAULT_TMOUT || access(tmpfile, F_OK) != 0) {
    PAC_LOGF(afl->pacfix_log, "[PacFuzz] [run_valuation] [timeout %d] [no-file %d] [time %llu]\n", fault_tmp == FAULT_TMOUT, access(tmpfile, F_OK) != 0, get_cur_time() - afl->start_time);
    ck_free(tmpfile);
    return 0;
  }

  u32 hash = hash_file(tmpfile);

  // Check if the hash is already in the hash table
  struct key_value_pair *kvp = hashmap_get(afl->value_map, hash);
  if (kvp) {
    PAC_LOGF(afl->pacfix_log, "[PacFuzz] [run_valuation] [hash %u] [already-exist] [time %llu]\n", hash, get_cur_time() - afl->start_time);
    remove(tmpfile);
    ck_free(tmpfile);
    return 0;
  }
  hashmap_insert(afl->value_map, hash, NULL);

  *valuation_file = tmpfile;
  *val_hash = hash;
  return 1;
}


void save_valuation(afl_state_t *afl, u32 val_hash, u8 *valuation_file, u8 crashed) {
  if (crashed) {
    afl->total_saved_crashes++;
  } else {
    afl->total_saved_positives++;
  }
  u8 *target_file = alloc_printf(
      "memory/%s/id:%06llu", crashed ? "neg" : "pos",
      crashed ? afl->total_saved_crashes : afl->total_saved_positives);
  u8 *target_file_full = alloc_printf("%s/%s", afl->out_dir, target_file);
  rename(valuation_file, target_file_full);
  ck_free(valuation_file);
  ck_free(target_file);
  ck_free(target_file_full);
}
