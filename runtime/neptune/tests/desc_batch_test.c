/* Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Drives the staged DESC_BATCH append helpers (npt_desc_stage.h) the
 * way npt_ring_stage_append_desc does: a byte buffer standing in for a
 * thread's stage, plus the last-batch offset the ring tracks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../npt_desc_stage.h"

#define CHECK(x) do { if (!(x)) { \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

#define CAP 16384u
#define DEV 0x1122334455667788ull

static uint8_t buf[CAP];
static uint32_t used;
static uint32_t off;
static struct npt_desc_copy_iv iv;

static void reset_stage(void)
{
   used = 0;
   off = NPT_DESC_BATCH_OFF_NONE;
   npt_desc_copy_iv_reset(&iv);
   memset(buf, 0, sizeof(buf));
}

/* Mirrors npt_ring_stage_append_desc: false when the entry would go
 * through the per-call submit instead of staging. */
static bool append_or_begin(uint32_t kind, uint64_t dev, uint32_t heap,
                            uint32_t inc, const void *entry, uint32_t es)
{
   if (kind == NPT_DESC_BATCH_COPY_SIMPLE &&
       es == sizeof(struct npt_desc_copy_entry)) {
      const struct npt_desc_copy_entry *e = entry;
      if (!inc)
         return false;
      const uint64_t dh = e->dst + (uint64_t)e->num * inc;
      const uint64_t sh = e->src + (uint64_t)e->num * inc;
      if (sh > e->dst && e->src < dh)
         return false;
   }
   if (off != NPT_DESC_BATCH_OFF_NONE &&
       npt_desc_stage_extend(buf, &used, CAP, off, kind, dev, heap, inc,
                             entry, es, &iv))
      return true;
   CHECK(npt_desc_stage_begin(buf, &used, CAP, kind, dev, heap, entry,
                              es, &off));
   if (kind == NPT_DESC_BATCH_COPY_SIMPLE &&
       es == sizeof(struct npt_desc_copy_entry))
      npt_desc_copy_iv_set(&iv, entry, inc);
   else
      npt_desc_copy_iv_reset(&iv);
   return true;
}

static struct npt_cmd_desc_batch batch_at(uint32_t batch_off)
{
   struct npt_cmd_desc_batch bh;
   memcpy(&bh, buf + batch_off, sizeof(bh));
   return bh;
}

static struct npt_desc_copy_entry copy_at(uint32_t batch_off, uint32_t i)
{
   struct npt_desc_copy_entry e;
   memcpy(&e, buf + batch_off + sizeof(struct npt_cmd_desc_batch) +
             i * sizeof(e), sizeof(e));
   return e;
}

static struct npt_desc_copy_entry cent(uint64_t dst, uint64_t src, uint32_t num)
{
   const struct npt_desc_copy_entry e = { dst, src, num, 0 };
   return e;
}

int main(void)
{
   /* Consecutive copies of one kind/device/heap coalesce into a single
    * batch command in place. */
   reset_stage();
   struct npt_desc_copy_entry a = cent(0x1000, 0x8000, 2);
   struct npt_desc_copy_entry b = cent(0x2000, 0x9000, 1);
   append_or_begin(NPT_DESC_BATCH_COPY_SIMPLE, DEV, 0, 16, &a, sizeof(a));
   append_or_begin(NPT_DESC_BATCH_COPY_SIMPLE, DEV, 0, 16, &b, sizeof(b));
   struct npt_cmd_desc_batch bh = batch_at(off);
   CHECK(bh.count == 2);
   CHECK(bh.header.cmd_size ==
         sizeof(struct npt_cmd_desc_batch) + 2 * sizeof(a));
   CHECK(used == bh.header.cmd_size);
   struct npt_desc_copy_entry r = copy_at(off, 1);
   CHECK(r.dst == 0x2000 && r.src == 0x9000 && r.num == 1);

   /* A contiguous run (dst and src both continue the previous range)
    * merges into the last entry instead of appending. */
   reset_stage();
   a = cent(0x1000, 0x8000, 2);      /* covers dst 0x1000..0x101f, inc 16 */
   b = cent(0x1020, 0x8020, 3);      /* contiguous in both */
   append_or_begin(NPT_DESC_BATCH_COPY_SIMPLE, DEV, 0, 16, &a, sizeof(a));
   append_or_begin(NPT_DESC_BATCH_COPY_SIMPLE, DEV, 0, 16, &b, sizeof(b));
   bh = batch_at(off);
   CHECK(bh.count == 1);
   r = copy_at(off, 0);
   CHECK(r.num == 5);
   CHECK(used == sizeof(struct npt_cmd_desc_batch) + sizeof(a));

   /* Only contiguous in dst: no merge. */
   reset_stage();
   b = cent(0x1020, 0x9000, 3);
   append_or_begin(NPT_DESC_BATCH_COPY_SIMPLE, DEV, 0, 16, &a, sizeof(a));
   append_or_begin(NPT_DESC_BATCH_COPY_SIMPLE, DEV, 0, 16, &b, sizeof(b));
   CHECK(batch_at(off).count == 2);

   /* Unknown increment: ranges cannot be bounded, so the entry goes
    * through the per-call submit instead of staging at all. */
   reset_stage();
   b = cent(0x1020, 0x8020, 3);
   CHECK(!append_or_begin(NPT_DESC_BATCH_COPY_SIMPLE, DEV, 0, 0,
                          &a, sizeof(a)));
   CHECK(!append_or_begin(NPT_DESC_BATCH_COPY_SIMPLE, DEV, 0, 0,
                          &b, sizeof(b)));
   CHECK(used == 0 && off == NPT_DESC_BATCH_OFF_NONE);

   /* Different kind, device or heap type closes the batch. */
   reset_stage();
   struct npt_desc_cbv_entry cbv = { 0x400000, 256, 1, 0x5000 };
   append_or_begin(NPT_DESC_BATCH_COPY_SIMPLE, DEV, 0, 16, &a, sizeof(a));
   const uint32_t first_off = off;
   append_or_begin(NPT_DESC_BATCH_CREATE_CBV, DEV, 0, 0, &cbv, sizeof(cbv));
   CHECK(off != first_off);
   CHECK(batch_at(off).kind == NPT_DESC_BATCH_CREATE_CBV);
   CHECK(batch_at(first_off).count == 1);
   append_or_begin(NPT_DESC_BATCH_COPY_SIMPLE, DEV + 1, 0, 16, &a, sizeof(a));
   CHECK(off != first_off);
   CHECK(batch_at(off).count == 1);

   /* A non-batch staged append clears the tracked offset (the ring does
    * this in npt_ring_stage_append); the next entry opens a new batch
    * rather than extending the closed one. */
   reset_stage();
   append_or_begin(NPT_DESC_BATCH_COPY_SIMPLE, DEV, 0, 16, &a, sizeof(a));
   const uint32_t closed_off = off;
   used += 8;                       /* stands in for a foreign command */
   off = NPT_DESC_BATCH_OFF_NONE;   /* what npt_ring_stage_append stores */
   npt_desc_copy_iv_reset(&iv);
   append_or_begin(NPT_DESC_BATCH_COPY_SIMPLE, DEV, 0, 16, &b, sizeof(b));
   CHECK(off != closed_off);
   CHECK(batch_at(closed_off).count == 1);
   CHECK(batch_at(off).count == 1);

   /* The 256-entry cap closes the batch. */
   reset_stage();
   for (unsigned i = 0; i < NPT_DESC_BATCH_MAX_ENTRIES; i++)
      append_or_begin(NPT_DESC_BATCH_CREATE_CBV, DEV, 0, 0, &cbv, sizeof(cbv));
   CHECK(batch_at(off).count == NPT_DESC_BATCH_MAX_ENTRIES);
   const uint32_t full_off = off;
   append_or_begin(NPT_DESC_BATCH_CREATE_CBV, DEV, 0, 0, &cbv, sizeof(cbv));
   CHECK(off != full_off);
   CHECK(batch_at(off).count == 1);

   /* No room to append an entry: extend fails; the caller flushes and
    * starts a new batch at 0. */
   reset_stage();
   append_or_begin(NPT_DESC_BATCH_CREATE_CBV, DEV, 0, 0, &cbv, sizeof(cbv));
   used = CAP - sizeof(cbv) + 1;
   CHECK(!npt_desc_stage_extend(buf, &used, CAP, off,
                                NPT_DESC_BATCH_CREATE_CBV, DEV, 0, 0,
                                &cbv, sizeof(cbv), &iv));
   used = 0;
   off = NPT_DESC_BATCH_OFF_NONE;   /* what a flush stores */
   append_or_begin(NPT_DESC_BATCH_CREATE_CBV, DEV, 0, 0, &cbv, sizeof(cbv));
   CHECK(off == 0 && batch_at(0).count == 1);

   /* A shift-by-one chain (each src is the previous dst): every copy
    * would read a slot an earlier copy wrote, so each opens a fresh
    * batch instead of joining or merging. */
   reset_stage();
   a = cent(0x110, 0x100, 1);
   append_or_begin(NPT_DESC_BATCH_COPY_SIMPLE, DEV, 0, 16, &a, sizeof(a));
   const uint32_t b1 = off;
   a = cent(0x120, 0x110, 1);
   append_or_begin(NPT_DESC_BATCH_COPY_SIMPLE, DEV, 0, 16, &a, sizeof(a));
   const uint32_t b2 = off;
   a = cent(0x130, 0x120, 1);
   append_or_begin(NPT_DESC_BATCH_COPY_SIMPLE, DEV, 0, 16, &a, sizeof(a));
   CHECK(b2 != b1 && off != b2);
   CHECK(batch_at(b1).count == 1);
   CHECK(batch_at(b2).count == 1);
   CHECK(batch_at(off).count == 1);

   /* RAW: the second src overlaps the first dst. */
   reset_stage();
   a = cent(0x1000, 0x8000, 2);
   append_or_begin(NPT_DESC_BATCH_COPY_SIMPLE, DEV, 0, 16, &a, sizeof(a));
   const uint32_t raw_off = off;
   a = cent(0x4000, 0x1008, 1);
   append_or_begin(NPT_DESC_BATCH_COPY_SIMPLE, DEV, 0, 16, &a, sizeof(a));
   CHECK(off != raw_off);
   CHECK(batch_at(raw_off).count == 1);
   CHECK(batch_at(off).count == 1);

   /* WAW: the second dst overlaps the first dst. */
   reset_stage();
   a = cent(0x1000, 0x8000, 2);
   append_or_begin(NPT_DESC_BATCH_COPY_SIMPLE, DEV, 0, 16, &a, sizeof(a));
   const uint32_t waw_off = off;
   a = cent(0x1010, 0x9000, 1);
   append_or_begin(NPT_DESC_BATCH_COPY_SIMPLE, DEV, 0, 16, &a, sizeof(a));
   CHECK(off != waw_off);
   CHECK(batch_at(off).count == 1);

   /* WAR: the second dst overlaps the first src. */
   reset_stage();
   a = cent(0x1000, 0x8000, 2);
   append_or_begin(NPT_DESC_BATCH_COPY_SIMPLE, DEV, 0, 16, &a, sizeof(a));
   const uint32_t war_off = off;
   a = cent(0x8008, 0x9000, 1);
   append_or_begin(NPT_DESC_BATCH_COPY_SIMPLE, DEV, 0, 16, &a, sizeof(a));
   CHECK(off != war_off);
   CHECK(batch_at(off).count == 1);

   /* A self-overlapping copy goes per-call; batching it would keep the
    * overlap inside the host's single CopyDescriptors. */
   reset_stage();
   a = cent(0x1008, 0x1000, 4);
   CHECK(!append_or_begin(NPT_DESC_BATCH_COPY_SIMPLE, DEV, 0, 16,
                          &a, sizeof(a)));
   CHECK(used == 0);

   /* A conflict opens a new batch whose intervals reset to the new
    * entry: a third copy disjoint from it but overlapping the first
    * batch still joins the second batch normally. */
   reset_stage();
   a = cent(0x1000, 0x8000, 2);
   append_or_begin(NPT_DESC_BATCH_COPY_SIMPLE, DEV, 0, 16, &a, sizeof(a));
   a = cent(0x1008, 0x9000, 1);   /* WAW vs the first batch */
   append_or_begin(NPT_DESC_BATCH_COPY_SIMPLE, DEV, 0, 16, &a, sizeof(a));
   const uint32_t second = off;
   a = cent(0x2000, 0xA000, 1);
   append_or_begin(NPT_DESC_BATCH_COPY_SIMPLE, DEV, 0, 16, &a, sizeof(a));
   CHECK(off == second);
   CHECK(batch_at(off).count == 2);

   puts("NPT_DESC_BATCH_TEST: PASS");
   return 0;
}
