/*
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "gc/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc/shenandoah/shenandoahFreeSet.hpp"
#include "gc/shenandoah/shenandoahGeneration.hpp"
#include "gc/shenandoah/shenandoahHeap.hpp"
#include "gc/shenandoah/shenandoahMarkClosures.hpp"
#include "gc/shenandoah/shenandoahMonitoringSupport.hpp"
#include "gc/shenandoah/shenandoahOldGeneration.hpp"
#include "gc/shenandoah/shenandoahReferenceProcessor.hpp"
#include "gc/shenandoah/shenandoahTaskqueue.inline.hpp"
#include "gc/shenandoah/shenandoahUtils.hpp"
#include "gc/shenandoah/shenandoahVerifier.hpp"
#include "gc/shenandoah/shenandoahYoungGeneration.hpp"
#include "gc/shenandoah/heuristics/shenandoahHeuristics.hpp"

#include "utilities/quickSort.hpp"

class ShenandoahResetUpdateRegionStateClosure : public ShenandoahHeapRegionClosure {
 private:
  ShenandoahHeap* _heap;
  ShenandoahMarkingContext* const _ctx;
 public:
  ShenandoahResetUpdateRegionStateClosure() :
    _heap(ShenandoahHeap::heap()),
    _ctx(_heap->marking_context()) {}

  void heap_region_do(ShenandoahHeapRegion* r) override {
    if (_heap->is_bitmap_slice_committed(r)) {
      _ctx->clear_bitmap(r);
    }

    if (r->is_active()) {
      // Reset live data and set TAMS optimistically. We would recheck these under the pause
      // anyway to capture any updates that happened since now.
      _ctx->capture_top_at_mark_start(r);
      r->clear_live_data();
    }
  }

  bool is_thread_safe() override { return true; }
};

class ShenandoahResetBitmapTask : public ShenandoahHeapRegionClosure {
 private:
  ShenandoahHeap* _heap;
  ShenandoahMarkingContext* const _ctx;
 public:
  ShenandoahResetBitmapTask() :
    _heap(ShenandoahHeap::heap()),
    _ctx(_heap->marking_context()) {}

  void heap_region_do(ShenandoahHeapRegion* region) {
    if (_heap->is_bitmap_slice_committed(region)) {
      _ctx->clear_bitmap(region);
    }
  }

  bool is_thread_safe() { return true; }
};

class ShenandoahMergeWriteTable: public ShenandoahHeapRegionClosure {
 private:
  ShenandoahHeap* _heap;
  RememberedScanner* _scanner;
 public:
  ShenandoahMergeWriteTable() : _heap(ShenandoahHeap::heap()), _scanner(_heap->card_scan()) {}

  virtual void heap_region_do(ShenandoahHeapRegion* r) override {
    if (r->is_old()) {
      _scanner->merge_write_table(r->bottom(), ShenandoahHeapRegion::region_size_words());
    }
  }

  virtual bool is_thread_safe() override {
    return true;
  }
};

class ShenandoahSquirrelAwayCardTable: public ShenandoahHeapRegionClosure {
 private:
  ShenandoahHeap* _heap;
  RememberedScanner* _scanner;
 public:
  ShenandoahSquirrelAwayCardTable() :
    _heap(ShenandoahHeap::heap()),
    _scanner(_heap->card_scan()) {}

  void heap_region_do(ShenandoahHeapRegion* region) {
    if (region->is_old()) {
      _scanner->reset_remset(region->bottom(), ShenandoahHeapRegion::region_size_words());
    }
  }

  bool is_thread_safe() { return true; }
};

void ShenandoahGeneration::confirm_heuristics_mode() {
  if (_heuristics->is_diagnostic() && !UnlockDiagnosticVMOptions) {
    vm_exit_during_initialization(
            err_msg("Heuristics \"%s\" is diagnostic, and must be enabled via -XX:+UnlockDiagnosticVMOptions.",
                    _heuristics->name()));
  }
  if (_heuristics->is_experimental() && !UnlockExperimentalVMOptions) {
    vm_exit_during_initialization(
            err_msg("Heuristics \"%s\" is experimental, and must be enabled via -XX:+UnlockExperimentalVMOptions.",
                    _heuristics->name()));
  }
}

ShenandoahHeuristics* ShenandoahGeneration::initialize_heuristics(ShenandoahMode* gc_mode) {
  _heuristics = gc_mode->initialize_heuristics(this);
  _heuristics->set_guaranteed_gc_interval(ShenandoahGuaranteedGCInterval);
  confirm_heuristics_mode();
  return _heuristics;
}

size_t ShenandoahGeneration::bytes_allocated_since_gc_start() const {
  return Atomic::load(&_bytes_allocated_since_gc_start);
}

void ShenandoahGeneration::reset_bytes_allocated_since_gc_start() {
  Atomic::store(&_bytes_allocated_since_gc_start, (size_t)0);
}

void ShenandoahGeneration::increase_allocated(size_t bytes) {
  Atomic::add(&_bytes_allocated_since_gc_start, bytes, memory_order_relaxed);
}

void ShenandoahGeneration::log_status(const char *msg) const {
  typedef LogTarget(Info, gc, ergo) LogGcInfo;

  if (!LogGcInfo::is_enabled()) {
    return;
  }

  // Not under a lock here, so read each of these once to make sure
  // byte size in proper unit and proper unit for byte size are consistent.
  size_t v_used = used();
  size_t v_used_regions = used_regions_size();
  size_t v_soft_max_capacity = soft_max_capacity();
  size_t v_max_capacity = max_capacity();
  size_t v_available = available();
  size_t v_humongous_waste = get_humongous_waste();
  LogGcInfo::print("%s: %s generation used: " SIZE_FORMAT "%s, used regions: " SIZE_FORMAT "%s, "
                   "humongous waste: " SIZE_FORMAT "%s, soft capacity: " SIZE_FORMAT "%s, max capacity: " SIZE_FORMAT "%s, "
                   "available: " SIZE_FORMAT "%s", msg, name(),
                   byte_size_in_proper_unit(v_used),              proper_unit_for_byte_size(v_used),
                   byte_size_in_proper_unit(v_used_regions),      proper_unit_for_byte_size(v_used_regions),
                   byte_size_in_proper_unit(v_humongous_waste),   proper_unit_for_byte_size(v_humongous_waste),
                   byte_size_in_proper_unit(v_soft_max_capacity), proper_unit_for_byte_size(v_soft_max_capacity),
                   byte_size_in_proper_unit(v_max_capacity),      proper_unit_for_byte_size(v_max_capacity),
                   byte_size_in_proper_unit(v_available),         proper_unit_for_byte_size(v_available));
}

void ShenandoahGeneration::reset_mark_bitmap() {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  heap->assert_gc_workers(heap->workers()->active_workers());

  set_mark_incomplete();

  ShenandoahResetBitmapTask task;
  parallel_heap_region_iterate(&task);
}

// The ideal is to swap the remembered set so the safepoint effort is no more than a few pointer manipulations.
// However, limitations in the implementation of the mutator write-barrier make it difficult to simply change the
// location of the card table.  So the interim implementation of swap_remembered_set will copy the write-table
// onto the read-table and will then clear the write-table.
void ShenandoahGeneration::swap_remembered_set() {
  // Must be sure that marking is complete before we swap remembered set.
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  heap->assert_gc_workers(heap->workers()->active_workers());
  shenandoah_assert_safepoint();

  // TODO: Eventually, we want replace this with a constant-time exchange of pointers.
  ShenandoahSquirrelAwayCardTable task;
  heap->old_generation()->parallel_heap_region_iterate(&task);
}

// If a concurrent cycle fails _after_ the card table has been swapped we need to update the read card
// table with any writes that have occurred during the transition to the degenerated cycle. Without this,
// newly created objects which are only referenced by old objects could be lost when the remembered set
// is scanned during the degenerated mark.
void ShenandoahGeneration::merge_write_table() {
  // This should only happen for degenerated cycles
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  heap->assert_gc_workers(heap->workers()->active_workers());
  shenandoah_assert_safepoint();

  ShenandoahMergeWriteTable task;
  heap->old_generation()->parallel_heap_region_iterate(&task);
}

void ShenandoahGeneration::prepare_gc() {
  // Invalidate the marking context
  set_mark_incomplete();

  // Capture Top At Mark Start for this generation (typically young) and reset mark bitmap.
  ShenandoahResetUpdateRegionStateClosure cl;
  parallel_heap_region_iterate(&cl);
}

inline void assert_no_in_place_promotions() {
#ifdef ASSERT
  class ShenandoahNoInPlacePromotions : public ShenandoahHeapRegionClosure {
  public:
    void heap_region_do(ShenandoahHeapRegion *r) override {
      assert(r->get_top_before_promote() == nullptr,
             "Region " SIZE_FORMAT " should not be ready for in-place promotion", r->index());
    }
  } cl;
  ShenandoahHeap::heap()->heap_region_iterate(&cl);
#endif
}

bool ShenandoahGeneration::is_bitmap_clear() {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  ShenandoahMarkingContext* context = heap->marking_context();
  size_t num_regions = heap->num_regions();
  for (size_t idx = 0; idx < num_regions; idx++) {
    ShenandoahHeapRegion* r = heap->get_region(idx);
    if (contains(r) && r->is_affiliated()) {
      if (heap->is_bitmap_slice_committed(r) && (context->top_at_mark_start(r) > r->bottom()) &&
          !context->is_bitmap_clear_range(r->bottom(), r->end())) {
        return false;
      }
    }
  }
  return true;
}

bool ShenandoahGeneration::is_mark_complete() {
  return _is_marking_complete.is_set();
}

void ShenandoahGeneration::set_mark_complete() {
  _is_marking_complete.set();
}

void ShenandoahGeneration::set_mark_incomplete() {
  _is_marking_complete.unset();
}

ShenandoahMarkingContext* ShenandoahGeneration::complete_marking_context() {
  assert(is_mark_complete(), "Marking must be completed.");
  return ShenandoahHeap::heap()->marking_context();
}

void ShenandoahGeneration::cancel_marking() {
  log_info(gc)("Cancel marking: %s", name());
  if (is_concurrent_mark_in_progress()) {
    set_mark_incomplete();
  }
  _task_queues->clear();
  ref_processor()->abandon_partial_discovery();
  set_concurrent_mark_in_progress(false);
}

ShenandoahGeneration::ShenandoahGeneration(ShenandoahGenerationType type,
                                           uint max_workers,
                                           size_t max_capacity,
                                           size_t soft_max_capacity) :
  _type(type),
  _task_queues(new ShenandoahObjToScanQueueSet(max_workers)),
  _ref_processor(new ShenandoahReferenceProcessor(MAX2(max_workers, 1U))),
  _affiliated_region_count(0), _humongous_waste(0), _used(0), _bytes_allocated_since_gc_start(0),
  _max_capacity(max_capacity), _soft_max_capacity(soft_max_capacity),
  _heuristics(nullptr) {
  _is_marking_complete.set();
  assert(max_workers > 0, "At least one queue");
  for (uint i = 0; i < max_workers; ++i) {
    ShenandoahObjToScanQueue* task_queue = new ShenandoahObjToScanQueue();
    _task_queues->register_queue(i, task_queue);
  }
}

ShenandoahGeneration::~ShenandoahGeneration() {
  for (uint i = 0; i < _task_queues->size(); ++i) {
    ShenandoahObjToScanQueue* q = _task_queues->queue(i);
    delete q;
  }
  delete _task_queues;
}

void ShenandoahGeneration::reserve_task_queues(uint workers) {
  _task_queues->reserve(workers);
}

ShenandoahObjToScanQueueSet* ShenandoahGeneration::old_gen_task_queues() const {
  return nullptr;
}

void ShenandoahGeneration::scan_remembered_set(bool is_concurrent) {
  assert(is_young(), "Should only scan remembered set for young generation.");

  ShenandoahHeap* const heap = ShenandoahHeap::heap();
  uint nworkers = heap->workers()->active_workers();
  reserve_task_queues(nworkers);

  ShenandoahReferenceProcessor* rp = ref_processor();
  ShenandoahRegionChunkIterator work_list(nworkers);
  ShenandoahScanRememberedTask task(task_queues(), old_gen_task_queues(), rp, &work_list, is_concurrent);
  heap->assert_gc_workers(nworkers);
  heap->workers()->run_task(&task);
  if (ShenandoahEnableCardStats) {
    assert(heap->card_scan() != nullptr, "Not generational");
    heap->card_scan()->log_card_stats(nworkers, CARD_STAT_SCAN_RS);
  }
}

size_t ShenandoahGeneration::increment_affiliated_region_count() {
  shenandoah_assert_heaplocked_or_fullgc_safepoint();
  // During full gc, multiple GC worker threads may change region affiliations without a lock.  No lock is enforced
  // on read and write of _affiliated_region_count.  At the end of full gc, a single thread overwrites the count with
  // a coherent value.
  _affiliated_region_count++;
  return _affiliated_region_count;
}

size_t ShenandoahGeneration::decrement_affiliated_region_count() {
  shenandoah_assert_heaplocked_or_fullgc_safepoint();
  // During full gc, multiple GC worker threads may change region affiliations without a lock.  No lock is enforced
  // on read and write of _affiliated_region_count.  At the end of full gc, a single thread overwrites the count with
  // a coherent value.
  _affiliated_region_count--;
  // TODO: REMOVE IS_GLOBAL() QUALIFIER AFTER WE FIX GLOBAL AFFILIATED REGION ACCOUNTING
  assert(is_global() || ShenandoahHeap::heap()->is_full_gc_in_progress() ||
         (_used + _humongous_waste <= _affiliated_region_count * ShenandoahHeapRegion::region_size_bytes()),
         "used + humongous cannot exceed regions");
  return _affiliated_region_count;
}

size_t ShenandoahGeneration::increase_affiliated_region_count(size_t delta) {
  shenandoah_assert_heaplocked_or_fullgc_safepoint();
  _affiliated_region_count += delta;
  return _affiliated_region_count;
}

size_t ShenandoahGeneration::decrease_affiliated_region_count(size_t delta) {
  shenandoah_assert_heaplocked_or_fullgc_safepoint();
  assert(_affiliated_region_count >= delta, "Affiliated region count cannot be negative");

  _affiliated_region_count -= delta;
  // TODO: REMOVE IS_GLOBAL() QUALIFIER AFTER WE FIX GLOBAL AFFILIATED REGION ACCOUNTING
  assert(is_global() || ShenandoahHeap::heap()->is_full_gc_in_progress() ||
         (_used + _humongous_waste <= _affiliated_region_count * ShenandoahHeapRegion::region_size_bytes()),
         "used + humongous cannot exceed regions");
  return _affiliated_region_count;
}

void ShenandoahGeneration::establish_usage(size_t num_regions, size_t num_bytes, size_t humongous_waste) {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "must be at a safepoint");
  _affiliated_region_count = num_regions;
  _used = num_bytes;
  _humongous_waste = humongous_waste;
}

void ShenandoahGeneration::increase_used(size_t bytes) {
  Atomic::add(&_used, bytes);
}

void ShenandoahGeneration::increase_humongous_waste(size_t bytes) {
  if (bytes > 0) {
    Atomic::add(&_humongous_waste, bytes);
  }
}

void ShenandoahGeneration::decrease_humongous_waste(size_t bytes) {
  if (bytes > 0) {
    assert(ShenandoahHeap::heap()->is_full_gc_in_progress() || (_humongous_waste >= bytes),
           "Waste (" SIZE_FORMAT ") cannot be negative (after subtracting " SIZE_FORMAT ")", _humongous_waste, bytes);
    Atomic::sub(&_humongous_waste, bytes);
  }
}

void ShenandoahGeneration::decrease_used(size_t bytes) {
  // TODO: REMOVE IS_GLOBAL() QUALIFIER AFTER WE FIX GLOBAL AFFILIATED REGION ACCOUNTING
  assert(is_global() || ShenandoahHeap::heap()->is_full_gc_in_progress() ||
         (_used >= bytes), "cannot reduce bytes used by generation below zero");
  Atomic::sub(&_used, bytes);
}

size_t ShenandoahGeneration::used_regions() const {
  return _affiliated_region_count;
}

size_t ShenandoahGeneration::free_unaffiliated_regions() const {
  size_t result = max_capacity() / ShenandoahHeapRegion::region_size_bytes();
  if (_affiliated_region_count > result) {
    result = 0;
  } else {
    result -= _affiliated_region_count;
  }
  return result;
}

size_t ShenandoahGeneration::used_regions_size() const {
  return _affiliated_region_count * ShenandoahHeapRegion::region_size_bytes();
}

size_t ShenandoahGeneration::available() const {
  return available(max_capacity());
}

size_t ShenandoahGeneration::soft_available() const {
  return available(soft_max_capacity());
}

size_t ShenandoahGeneration::available(size_t capacity) const {
  size_t in_use = used() + get_humongous_waste();
  return in_use > capacity ? 0 : capacity - in_use;
}

void ShenandoahGeneration::increase_capacity(size_t increment) {
  shenandoah_assert_heaplocked_or_safepoint();

  // We do not enforce that new capacity >= heap->max_size_for(this).  The maximum generation size is treated as a rule of thumb
  // which may be violated during certain transitions, such as when we are forcing transfers for the purpose of promoting regions
  // in place.
  // TODO: REMOVE IS_GLOBAL() QUALIFIER AFTER WE FIX GLOBAL AFFILIATED REGION ACCOUNTING
  assert(is_global() || ShenandoahHeap::heap()->is_full_gc_in_progress() ||
         (_max_capacity + increment <= ShenandoahHeap::heap()->max_capacity()), "Generation cannot be larger than heap size");
  assert(increment % ShenandoahHeapRegion::region_size_bytes() == 0, "Generation capacity must be multiple of region size");
  _max_capacity += increment;

  // This detects arithmetic wraparound on _used
  // TODO: REMOVE IS_GLOBAL() QUALIFIER AFTER WE FIX GLOBAL AFFILIATED REGION ACCOUNTING
  assert(is_global() || ShenandoahHeap::heap()->is_full_gc_in_progress() ||
         (_affiliated_region_count * ShenandoahHeapRegion::region_size_bytes() >= _used),
         "Affiliated regions must hold more than what is currently used");
}

void ShenandoahGeneration::decrease_capacity(size_t decrement) {
  shenandoah_assert_heaplocked_or_safepoint();

  // We do not enforce that new capacity >= heap->min_size_for(this).  The minimum generation size is treated as a rule of thumb
  // which may be violated during certain transitions, such as when we are forcing transfers for the purpose of promoting regions
  // in place.
  assert(decrement % ShenandoahHeapRegion::region_size_bytes() == 0, "Generation capacity must be multiple of region size");
  assert(_max_capacity >= decrement, "Generation capacity cannot be negative");

  _max_capacity -= decrement;

  // This detects arithmetic wraparound on _used
  // TODO: REMOVE IS_GLOBAL() QUALIFIER AFTER WE FIX GLOBAL AFFILIATED REGION ACCOUNTING
  assert(is_global() || ShenandoahHeap::heap()->is_full_gc_in_progress() ||
         (_affiliated_region_count * ShenandoahHeapRegion::region_size_bytes() >= _used),
         "Affiliated regions must hold more than what is currently used");
  // TODO: REMOVE IS_GLOBAL() QUALIFIER AFTER WE FIX GLOBAL AFFILIATED REGION ACCOUNTING
  assert(is_global() || ShenandoahHeap::heap()->is_full_gc_in_progress() ||
         (_used <= _max_capacity), "Cannot use more than capacity");
  // TODO: REMOVE IS_GLOBAL() QUALIFIER AFTER WE FIX GLOBAL AFFILIATED REGION ACCOUNTING
  assert(is_global() || ShenandoahHeap::heap()->is_full_gc_in_progress() ||
         (_affiliated_region_count * ShenandoahHeapRegion::region_size_bytes() <= _max_capacity),
         "Cannot use more than capacity");
}

void ShenandoahGeneration::record_success_concurrent(bool abbreviated) {
  heuristics()->record_success_concurrent(abbreviated);
  ShenandoahHeap::heap()->shenandoah_policy()->record_success_concurrent();
}

void ShenandoahGeneration::record_success_degenerated() {
  heuristics()->record_success_degenerated();
  ShenandoahHeap::heap()->shenandoah_policy()->record_success_degenerated();
}
