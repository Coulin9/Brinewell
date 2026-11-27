/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Staged DESC_BATCH append logic.  npt_ring.c wraps this in the stage
 * lock; keeping it lock-free in a header lets the unit test drive it
 * without the rest of the ring.  All staged bytes go through memcpy --
 * commands land at arbitrary offsets in the stage buffer.
 */

#ifndef NPT_DESC_STAGE_H
#define NPT_DESC_STAGE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "neptune-protocol/npt_protocol_transport.h"

/* Offset of the last staged DESC_BATCH header; UINT32_MAX when the last
 * staged command is not a batch. */
#define NPT_DESC_BATCH_OFF_NONE UINT32_MAX

/* Conservative bounding intervals ([lo,hi), in descriptor-handle
 * address space) over every entry of the open COPY_SIMPLE batch.  The
 * host replays the whole batch as a single CopyDescriptors call, in
 * which overlapping ranges are undefined, while the original program
 * order was a sequence of separate calls where a later copy may legally
 * read or rewrite slots an earlier one touched.  An entry joins the
 * batch only when its own ranges are disjoint from both intervals. */
struct npt_desc_copy_iv {
   uint64_t dst_lo, dst_hi, src_lo, src_hi;
};

static inline void
npt_desc_copy_iv_reset(struct npt_desc_copy_iv *iv)
{
   memset(iv, 0, sizeof(*iv));
}

static inline void
npt_desc_copy_iv_set(struct npt_desc_copy_iv *iv,
                     const struct npt_desc_copy_entry *e, uint64_t inc)
{
   iv->dst_lo = e->dst;
   iv->dst_hi = e->dst + (uint64_t)e->num * inc;
   iv->src_lo = e->src;
   iv->src_hi = e->src + (uint64_t)e->num * inc;
}

static inline void
npt_desc_copy_iv_grow(struct npt_desc_copy_iv *iv,
                      const struct npt_desc_copy_entry *e, uint64_t inc)
{
   const uint64_t dh = e->dst + (uint64_t)e->num * inc;
   const uint64_t sh = e->src + (uint64_t)e->num * inc;
   if (e->dst < iv->dst_lo)
      iv->dst_lo = e->dst;
   if (dh > iv->dst_hi)
      iv->dst_hi = dh;
   if (e->src < iv->src_lo)
      iv->src_lo = e->src;
   if (sh > iv->src_hi)
      iv->src_hi = sh;
}

/* True when the candidate entry's ranges are disjoint from the batch's
 * dst interval on the src side, and from both intervals on the dst
 * side.  False means the entry must start a new batch. */
static inline bool
npt_desc_copy_iv_clear(const struct npt_desc_copy_iv *iv,
                       const struct npt_desc_copy_entry *e, uint64_t inc)
{
   const uint64_t dh = e->dst + (uint64_t)e->num * inc;
   const uint64_t sh = e->src + (uint64_t)e->num * inc;
   if (sh > iv->dst_lo && e->src < iv->dst_hi)
      return false;
   if (dh > iv->dst_lo && e->dst < iv->dst_hi)
      return false;
   if (dh > iv->src_lo && e->dst < iv->src_hi)
      return false;
   return true;
}

/* Extend the batch at batch_off with entry.  A COPY_SIMPLE entry joins
 * only when npt_desc_copy_iv_clear holds for it, merges into the last
 * entry when both dst and src continue it contiguously and the merged
 * entry's own ranges stay disjoint, and otherwise appends; merge_inc is
 * the descriptor increment for heap_type and must be nonzero for
 * COPY_SIMPLE so the ranges can be bounded at all.  False when the
 * batch differs in kind, device or heap type, holds
 * NPT_DESC_BATCH_MAX_ENTRIES already, the COPY_SIMPLE entry cannot be
 * bounded or conflicts with the batch intervals, or the entry does not
 * fit; the caller then starts a new batch (or falls back per-call). */
static inline bool
npt_desc_stage_extend(uint8_t *buf, uint32_t *used_io, uint32_t capacity,
                      uint32_t batch_off, uint32_t kind, uint64_t device_id,
                      uint32_t heap_type, uint32_t merge_inc,
                      const void *entry, uint32_t entry_size,
                      struct npt_desc_copy_iv *iv)
{
   struct npt_cmd_desc_batch bh;
   memcpy(&bh, buf + batch_off, sizeof(bh));
   if (bh.kind != kind || bh.device_id != device_id ||
       bh.heap_type != heap_type || bh.count >= NPT_DESC_BATCH_MAX_ENTRIES)
      return false;
   const uint32_t used = *used_io;
   if (kind == NPT_DESC_BATCH_COPY_SIMPLE) {
      if (!merge_inc || entry_size != sizeof(struct npt_desc_copy_entry) ||
          !iv || !npt_desc_copy_iv_clear(iv, entry, merge_inc))
         return false;
      const uint32_t last_off =
         batch_off + (uint32_t)sizeof(bh) +
         (bh.count - 1u) * (uint32_t)sizeof(struct npt_desc_copy_entry);
      struct npt_desc_copy_entry last;
      memcpy(&last, buf + last_off, sizeof(last));
      const struct npt_desc_copy_entry *e = entry;
      if (e->num <= UINT32_MAX - last.num &&
          last.dst + (uint64_t)last.num * merge_inc == e->dst &&
          last.src + (uint64_t)last.num * merge_inc == e->src) {
         const uint32_t num = last.num + e->num;
         const uint64_t mdh = last.dst + (uint64_t)num * merge_inc;
         const uint64_t msh = last.src + (uint64_t)num * merge_inc;
         if (!(msh > last.dst && last.src < mdh)) {
            last.num = num;
            memcpy(buf + last_off, &last, sizeof(last));
            npt_desc_copy_iv_grow(iv, e, merge_inc);
            return true;
         }
      }
      if (used + entry_size > capacity)
         return false;
      memcpy(buf + used, entry, entry_size);
      bh.count++;
      bh.header.cmd_size += entry_size;
      memcpy(buf + batch_off, &bh, sizeof(bh));
      *used_io = used + entry_size;
      npt_desc_copy_iv_grow(iv, e, merge_inc);
      return true;
   }
   if (used + entry_size > capacity)
      return false;
   memcpy(buf + used, entry, entry_size);
   bh.count++;
   bh.header.cmd_size += entry_size;
   memcpy(buf + batch_off, &bh, sizeof(bh));
   *used_io = used + entry_size;
   return true;
}

/* Write a fresh batch (header + first entry) at *used_io.  False when
 * it does not fit; the caller flushes first. */
static inline bool
npt_desc_stage_begin(uint8_t *buf, uint32_t *used_io, uint32_t capacity,
                     uint32_t kind, uint64_t device_id, uint32_t heap_type,
                     const void *entry, uint32_t entry_size,
                     uint32_t *out_off)
{
   const uint32_t need =
      (uint32_t)sizeof(struct npt_cmd_desc_batch) + entry_size;
   const uint32_t off = *used_io;
   if (off + need > capacity)
      return false;
   const struct npt_cmd_desc_batch bh = {
      .header = {
         .cmd_type = NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_DESC,
                                            NPT_TRANSPORT_DESC_BATCH),
         .cmd_size = need,
      },
      .device_id = device_id,
      .kind = kind,
      .heap_type = heap_type,
      .count = 1,
   };
   memcpy(buf + off, &bh, sizeof(bh));
   memcpy(buf + off + sizeof(bh), entry, entry_size);
   *used_io = off + need;
   *out_off = off;
   return true;
}

#endif /* NPT_DESC_STAGE_H */
