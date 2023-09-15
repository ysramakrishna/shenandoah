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

#include "gc/shenandoah/mode/shenandoahMode.hpp"
#include "gc/shenandoah/shenandoahFreeSet.hpp"
#include "gc/shenandoah/shenandoahGeneration.hpp"
#include "gc/shenandoah/shenandoahGenerationalHeap.hpp"
// #include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahInitLogger.hpp"
#include "gc/shenandoah/shenandoahMarkClosures.hpp"
#include "gc/shenandoah/shenandoahOldGeneration.hpp"
#include "gc/shenandoah/shenandoahUtils.hpp"
#include "gc/shenandoah/shenandoahYoungGeneration.hpp"

#include "logging/log.hpp"

#include "utilities/quickSort.hpp"

class ShenandoahGenerationalInitLogger : public ShenandoahInitLogger {
public:
  static void print() {
    ShenandoahGenerationalInitLogger logger;
    logger.print_all();
  }

  void print_heap() override {
    ShenandoahInitLogger::print_heap();

    ShenandoahGenerationalHeap* gen_heap = ShenandoahGenerationalHeap::gen_heap();

    ShenandoahYoungGeneration* young = gen_heap->young_generation();
    log_info(gc, init)("Young Generation Soft Size: " PROPERFMT, PROPERFMTARGS(young->soft_max_capacity()));
    log_info(gc, init)("Young Generation Max: " PROPERFMT, PROPERFMTARGS(young->max_capacity()));

    ShenandoahOldGeneration* old = gen_heap->old_generation();
    log_info(gc, init)("Old Generation Soft Size: " PROPERFMT, PROPERFMTARGS(old->soft_max_capacity()));
    log_info(gc, init)("Old Generation Max: " PROPERFMT, PROPERFMTARGS(old->max_capacity()));
  }

protected:
  void print_gc_specific() override {
    ShenandoahInitLogger::print_gc_specific();

    ShenandoahGenerationalHeap* gen_heap = ShenandoahGenerationalHeap::gen_heap();
    log_info(gc, init)("Young Heuristics: %s", gen_heap->young_generation()->heuristics()->name());
    log_info(gc, init)("Old Heuristics: %s", gen_heap->old_generation()->heuristics()->name());
  }
};

ShenandoahGenerationalHeap::ShenandoahGenerationalHeap(ShenandoahCollectorPolicy* policy) :
  ShenandoahHeap(policy),
  _prepare_for_old_mark(false),
  _promotion_potential(0),
  _promotion_in_place_potential(0),
  _pad_for_promote_in_place(0),
  _promotable_humongous_regions(0),
  _promotable_humongous_usage(0),
  _regular_regions_promoted_in_place(0),
  _regular_usage_promoted_in_place(0),
  _promoted_reserve(0),
  _promoted_expended(0),
  _old_evac_reserve(0),
  _old_evac_expended(0),
  _young_evac_reserve(0),
  _captured_old_usage(0),
  _previous_promotion(0),
  _age_census(nullptr),
  _generation_sizer(mmu_tracker()),
  _young_generation(nullptr),
  _old_generation(nullptr),
  _young_gen_memory_pool(nullptr),
  _old_gen_memory_pool(nullptr),
  _old_regions_surplus(0),
  _old_regions_deficit(0),
  _card_scan(nullptr)
{
  //
  // Create the card table
  //
  assert(mode()->is_generational(), "Error");
  ShenandoahDirectCardMarkRememberedSet *rs;
  ShenandoahCardTable* card_table = ShenandoahBarrierSet::barrier_set()->card_table();
  size_t card_count = card_table->cards_required(max_capacity() / HeapWordSize);
  rs = new ShenandoahDirectCardMarkRememberedSet(ShenandoahBarrierSet::barrier_set()->card_table(), card_count);
  _card_scan = new ShenandoahScanRemembered<ShenandoahDirectCardMarkRememberedSet>(rs);
  
  // Age census structure
  _age_census = new ShenandoahAgeCensus();
}


/* 
ShenandoahGenerationalHeap* ShenandoahGenerationalHeap::heap() {
  CollectedHeap* heap = Universe::heap();
  return checked_cast<ShenandoahGenerationalHeap*>(heap);
}
*/

void ShenandoahGenerationalHeap::print_init_logger() const {
  ShenandoahGenerationalInitLogger logger;
  logger.print_all();
}

size_t ShenandoahGenerationalHeap::max_size_for(ShenandoahGeneration* generation) const {
  switch (generation->type()) {
    case YOUNG:
      return _generation_sizer.max_young_size();
    case OLD:
      return max_capacity() - _generation_sizer.min_young_size();
    case GLOBAL_GEN:
    case GLOBAL_NON_GEN:
      return max_capacity();
    default:
      ShouldNotReachHere();
      return 0;
  }
}

size_t ShenandoahGenerationalHeap::min_size_for(ShenandoahGeneration* generation) const {
  switch (generation->type()) {
    case YOUNG:
      return _generation_sizer.min_young_size();
    case OLD:
      return max_capacity() - _generation_sizer.max_young_size();
    case GLOBAL_GEN:
    case GLOBAL_NON_GEN:
      return min_capacity();
    default:
      ShouldNotReachHere();
      return 0;
  }
}

void ShenandoahGenerationalHeap::initialize_heuristics() {

  // Call superclass method
  ShenandoahHeap::initialize_heuristics();

  ShenandoahMode* gc_mode = mode();
  assert(gc_mode->is_generational(), "Error");

  // Max capacity is the maximum _allowed_ capacity. That is, the maximum allowed capacity
  // for old would be total heap - minimum capacity of young. This means the sum of the maximum
  // allowed for old and young could exceed the total heap size. It remains the case that the
  // _actual_ capacity of young + old = total.
  _generation_sizer.heap_size_changed(max_capacity());
  size_t initial_capacity_young = _generation_sizer.max_young_size();
  size_t max_capacity_young = _generation_sizer.max_young_size();
  size_t initial_capacity_old = max_capacity() - max_capacity_young;
  size_t max_capacity_old = max_capacity() - initial_capacity_young;

  _young_generation = new ShenandoahYoungGeneration(max_workers(), max_capacity_young, initial_capacity_young);
  _old_generation = new ShenandoahOldGeneration(max_workers(), max_capacity_old, initial_capacity_old);
  assert(global_generation() != nullptr, "Should have been initialized by superclass");
  assert(global_generation()->heuristics() != nullptr, "Should have been initialized by superclass");
  _young_generation->initialize_heuristics(gc_mode);
  _old_generation->initialize_heuristics(gc_mode);
}

ShenandoahOldHeuristics* ShenandoahGenerationalHeap::old_heuristics() {
  return (ShenandoahOldHeuristics*) _old_generation->heuristics();
}

ShenandoahYoungHeuristics* ShenandoahGenerationalHeap::young_heuristics() {
  return (ShenandoahYoungHeuristics*) _young_generation->heuristics();
}

bool ShenandoahGenerationalHeap::doing_mixed_evacuations() {
  return _old_generation->state() == ShenandoahOldGeneration::WAITING_FOR_EVAC;
}

bool ShenandoahGenerationalHeap::is_old_bitmap_stable() const {
  return _old_generation->is_mark_complete();
}

void ShenandoahGenerationalHeap::handle_old_evacuation(HeapWord* obj, size_t words, bool promotion) {
  // Only register the copy of the object that won the evacuation race.
  card_scan()->register_object_without_lock(obj);

  // Mark the entire range of the evacuated object as dirty.  At next remembered set scan,
  // we will clear dirty bits that do not hold interesting pointers.  It's more efficient to
  // do this in batch, in a background GC thread than to try to carefully dirty only cards
  // that hold interesting pointers right now.
  card_scan()->mark_range_as_dirty(obj, words);

  if (promotion) {
    // This evacuation was a promotion, track this as allocation against old gen
    old_generation()->increase_allocated(words * HeapWordSize);
  }
}

void ShenandoahGenerationalHeap::handle_old_evacuation_failure() {
  if (_old_gen_oom_evac.try_set()) {
    log_info(gc)("Old gen evac failure.");
  }
}

void ShenandoahGenerationalHeap::report_promotion_failure(Thread* thread, size_t size) {
  // We squelch excessive reports to reduce noise in logs.
  const size_t MaxReportsPerEpoch = 4;
  static size_t last_report_epoch = 0;
  static size_t epoch_report_count = 0;

  size_t promotion_reserve;
  size_t promotion_expended;

  size_t gc_id = control_thread()->get_gc_id();

  if ((gc_id != last_report_epoch) || (epoch_report_count++ < MaxReportsPerEpoch)) {
    {
      // Promotion failures should be very rare.  Invest in providing useful diagnostic info.
      ShenandoahHeapLocker locker(lock());
      promotion_reserve = get_promoted_reserve();
      promotion_expended = get_promoted_expended();
    }
    PLAB* plab = ShenandoahThreadLocalData::plab(thread);
    size_t words_remaining = (plab == nullptr)? 0: plab->words_remaining();
    const char* promote_enabled = ShenandoahThreadLocalData::allow_plab_promotions(thread)? "enabled": "disabled";
    ShenandoahGeneration* old_gen = old_generation();
    size_t old_capacity = old_gen->max_capacity();
    size_t old_usage = old_gen->used();
    size_t old_free_regions = old_gen->free_unaffiliated_regions();

    log_info(gc, ergo)("Promotion failed, size " SIZE_FORMAT ", has plab? %s, PLAB remaining: " SIZE_FORMAT
                       ", plab promotions %s, promotion reserve: " SIZE_FORMAT ", promotion expended: " SIZE_FORMAT
                       ", old capacity: " SIZE_FORMAT ", old_used: " SIZE_FORMAT ", old unaffiliated regions: " SIZE_FORMAT,
                       size * HeapWordSize, plab == nullptr? "no": "yes",
                       words_remaining * HeapWordSize, promote_enabled, promotion_reserve, promotion_expended,
                       old_capacity, old_usage, old_free_regions);

    if ((gc_id == last_report_epoch) && (epoch_report_count >= MaxReportsPerEpoch)) {
      log_info(gc, ergo)("Squelching additional promotion failure reports for current epoch");
    } else if (gc_id != last_report_epoch) {
      last_report_epoch = gc_id;;
      epoch_report_count = 1;
    }
  }
}

// Establish a new PLAB and allocate size HeapWords within it.
HeapWord* ShenandoahGenerationalHeap::allocate_from_plab_slow(Thread* thread, size_t size, bool is_promotion) {
  // New object should fit the PLAB size
  size_t min_size = MAX2(size, PLAB::min_size());

  // Figure out size of new PLAB, looking back at heuristics. Expand aggressively.
  size_t cur_size = ShenandoahThreadLocalData::plab_size(thread);
  if (cur_size == 0) {
    cur_size = PLAB::min_size();
  }
  size_t future_size = cur_size * 2;
  // Limit growth of PLABs to ShenandoahMaxEvacLABRatio * the minimum size.  This enables more equitable distribution of
  // available evacuation buidget between the many threads that are coordinating in the evacuation effort.
  if (ShenandoahMaxEvacLABRatio > 0) {
    future_size = MIN2(future_size, PLAB::min_size() * ShenandoahMaxEvacLABRatio);
  }
  future_size = MIN2(future_size, PLAB::max_size());
  future_size = MAX2(future_size, PLAB::min_size());

  size_t unalignment = future_size % CardTable::card_size_in_words();
  if (unalignment != 0) {
    future_size = future_size - unalignment + CardTable::card_size_in_words();
  }

  // Record new heuristic value even if we take any shortcut. This captures
  // the case when moderately-sized objects always take a shortcut. At some point,
  // heuristics should catch up with them.  Note that the requested cur_size may
  // not be honored, but we remember that this is the preferred size.
  ShenandoahThreadLocalData::set_plab_size(thread, future_size);
  if (cur_size < size) {
    // The PLAB to be allocated is still not large enough to hold the object. Fall back to shared allocation.
    // This avoids retiring perfectly good PLABs in order to represent a single large object allocation.
    return nullptr;
  }

  // Retire current PLAB, and allocate a new one.
  PLAB* plab = ShenandoahThreadLocalData::plab(thread);
  if (plab->words_remaining() < PLAB::min_size()) {
    // Retire current PLAB, and allocate a new one.
    // CAUTION: retire_plab may register the remnant filler object with the remembered set scanner without a lock.  This
    // is safe iff it is assured that each PLAB is a whole-number multiple of card-mark memory size and each PLAB is
    // aligned with the start of a card's memory range.
    retire_plab(plab, thread);

    size_t actual_size = 0;
    // allocate_new_plab resets plab_evacuated and plab_promoted and disables promotions if old-gen available is
    // less than the remaining evacuation need.  It also adjusts plab_preallocated and expend_promoted if appropriate.
    HeapWord* plab_buf = allocate_new_plab(min_size, cur_size, &actual_size);
    if (plab_buf == nullptr) {
      if (min_size == PLAB::min_size()) {
        // Disable plab promotions for this thread because we cannot even allocate a plab of minimal size.  This allows us
        // to fail faster on subsequent promotion attempts.
        ShenandoahThreadLocalData::disable_plab_promotions(thread);
      }
      return NULL;
    } else {
      ShenandoahThreadLocalData::enable_plab_retries(thread);
    }
    assert (size <= actual_size, "allocation should fit");
    if (ZeroTLAB) {
      // ..and clear it.
      Copy::zero_to_words(plab_buf, actual_size);
    } else {
      // ...and zap just allocated object.
#ifdef ASSERT
      // Skip mangling the space corresponding to the object header to
      // ensure that the returned space is not considered parsable by
      // any concurrent GC thread.
      size_t hdr_size = oopDesc::header_size();
      Copy::fill_to_words(plab_buf + hdr_size, actual_size - hdr_size, badHeapWordVal);
#endif // ASSERT
    }
    plab->set_buf(plab_buf, actual_size);
    if (is_promotion && !ShenandoahThreadLocalData::allow_plab_promotions(thread)) {
      return nullptr;
    }
    return plab->allocate(size);
  } else {
    // If there's still at least min_size() words available within the current plab, don't retire it.  Let's gnaw
    // away on this plab as long as we can.  Meanwhile, return nullptr to force this particular allocation request
    // to be satisfied with a shared allocation.  By packing more promotions into the previously allocated PLAB, we
    // reduce the likelihood of evacuation failures, and we we reduce the need for downsizing our PLABs.
    return nullptr;
  }
}

// TODO: It is probably most efficient to register all objects (both promotions and evacuations) that were allocated within
// this plab at the time we retire the plab.  A tight registration loop will run within both code and data caches.  This change
// would allow smaller and faster in-line implementation of alloc_from_plab().  Since plabs are aligned on card-table boundaries,
// this object registration loop can be performed without acquiring a lock.
void ShenandoahGenerationalHeap::retire_plab(PLAB* plab, Thread* thread) {
  // We don't enforce limits on plab_evacuated.  We let it consume all available old-gen memory in order to reduce
  // probability of an evacuation failure.  We do enforce limits on promotion, to make sure that excessive promotion
  // does not result in an old-gen evacuation failure.  Note that a failed promotion is relatively harmless.  Any
  // object that fails to promote in the current cycle will be eligible for promotion in a subsequent cycle.

  // When the plab was instantiated, its entirety was treated as if the entire buffer was going to be dedicated to
  // promotions.  Now that we are retiring the buffer, we adjust for the reality that the plab is not entirely promotions.
  //  1. Some of the plab may have been dedicated to evacuations.
  //  2. Some of the plab may have been abandoned due to waste (at the end of the plab).
  size_t not_promoted =
    ShenandoahThreadLocalData::get_plab_preallocated_promoted(thread) - ShenandoahThreadLocalData::get_plab_promoted(thread);
  ShenandoahThreadLocalData::reset_plab_promoted(thread);
  ShenandoahThreadLocalData::reset_plab_evacuated(thread);
  ShenandoahThreadLocalData::set_plab_preallocated_promoted(thread, 0);
  if (not_promoted > 0) {
    unexpend_promoted(not_promoted);
  }
  size_t waste = plab->waste();
  HeapWord* top = plab->top();
  plab->retire();
  if (top != nullptr && plab->waste() > waste && is_in_old(top)) {
    // If retiring the plab created a filler object, then we
    // need to register it with our card scanner so it can
    // safely walk the region backing the plab.
    log_debug(gc)("retire_plab() is registering remnant of size " SIZE_FORMAT " at " PTR_FORMAT,
                  plab->waste() - waste, p2i(top));
    card_scan()->register_object_without_lock(top);
  }
}

void ShenandoahGenerationalHeap::retire_plab(PLAB* plab) {
  Thread* thread = Thread::current();
  retire_plab(plab, thread);
}

void ShenandoahGenerationalHeap::cancel_old_gc() {
  shenandoah_assert_safepoint();
  assert(_old_generation != nullptr, "Should only have mixed collections in generation mode.");
  log_info(gc)("Terminating old gc cycle.");

  // Stop marking
  old_generation()->cancel_marking();
  // Stop coalescing undead objects
  set_prepare_for_old_mark_in_progress(false);
  // Stop tracking old regions
  old_heuristics()->abandon_collection_candidates();
  // Remove old generation access to young generation mark queues
  young_generation()->set_old_gen_task_queues(nullptr);
  // Transition to IDLE now.
  _old_generation->transition_to(ShenandoahOldGeneration::IDLE);
}

bool ShenandoahGenerationalHeap::is_old_gc_active() {
  return _old_generation->state() != ShenandoahOldGeneration::IDLE;
}

// xfer_limit is the maximum we're able to transfer from young to old
void ShenandoahGenerationalHeap::adjust_generation_sizes_for_next_cycle(
  size_t xfer_limit, size_t young_cset_regions, size_t old_cset_regions) {

  // Make sure old-generation is large enough, but no larger, than is necessary to hold mixed evacuations
  // and promotions if we anticipate either.
  size_t region_size_bytes = ShenandoahHeapRegion::region_size_bytes();
  size_t promo_load = get_promotion_potential();
  // The free set will reserve this amount of memory to hold young evacuations
  size_t young_reserve = (young_generation()->max_capacity() * ShenandoahEvacReserve) / 100;
  size_t old_reserve = 0;
  size_t mixed_candidates = old_heuristics()->unprocessed_old_collection_candidates();
  bool doing_mixed = (mixed_candidates > 0);
  bool doing_promotions = promo_load > 0;

  // round down
  size_t max_old_region_xfer = xfer_limit / region_size_bytes;

  // We can limit the reserve to the size of anticipated promotions
  size_t max_old_reserve = young_reserve * ShenandoahOldEvacRatioPercent / (100 - ShenandoahOldEvacRatioPercent);
  // Here's the algebra:
  //  TotalEvacuation = OldEvacuation + YoungEvacuation
  //  OldEvacuation = TotalEvacuation*(ShenandoahOldEvacRatioPercent/100)
  //  OldEvacuation = YoungEvacuation * (ShenandoahOldEvacRatioPercent/100)/(1 - ShenandoahOldEvacRatioPercent/100)
  //  OldEvacuation = YoungEvacuation * ShenandoahOldEvacRatioPercent/(100 - ShenandoahOldEvacRatioPercent)

  size_t reserve_for_mixed, reserve_for_promo;
  if (doing_mixed) {
    assert(old_generation()->available() >= old_generation()->free_unaffiliated_regions() * region_size_bytes,
           "Unaffiliated available must be less than total available");

    // We want this much memory to be unfragmented in order to reliably evacuate old.  This is conservative because we
    // may not evacuate the entirety of unprocessed candidates in a single mixed evacuation.
    size_t max_evac_need = (size_t)
      (old_heuristics()->unprocessed_old_collection_candidates_live_memory() * ShenandoahOldEvacWaste);
    size_t old_fragmented_available =
      old_generation()->available() - old_generation()->free_unaffiliated_regions() * region_size_bytes;
    reserve_for_mixed = max_evac_need + old_fragmented_available;
    if (reserve_for_mixed > max_old_reserve) {
      reserve_for_mixed = max_old_reserve;
    }
  } else {
    reserve_for_mixed = 0;
  }

  size_t available_for_promotions = max_old_reserve - reserve_for_mixed;
  if (doing_promotions) {
    // We're only promoting and we have a maximum bound on the amount to be promoted
    reserve_for_promo = (size_t) (promo_load * ShenandoahPromoEvacWaste);
    if (reserve_for_promo > available_for_promotions) {
      reserve_for_promo = available_for_promotions;
    }
  } else {
    reserve_for_promo = 0;
  }
  old_reserve = reserve_for_mixed + reserve_for_promo;
  assert(old_reserve <= max_old_reserve, "cannot reserve more than max for old evacuations");
  size_t old_available = old_generation()->available() + old_cset_regions * region_size_bytes;
  size_t young_available = young_generation()->available() + young_cset_regions * region_size_bytes;
  size_t old_region_deficit = 0;
  size_t old_region_surplus = 0;
  if (old_available >= old_reserve) {
    size_t old_excess = old_available - old_reserve;
    size_t excess_regions = old_excess / region_size_bytes;
    size_t unaffiliated_old_regions = old_generation()->free_unaffiliated_regions() + old_cset_regions;
    size_t unaffiliated_old = unaffiliated_old_regions * region_size_bytes;
    if (unaffiliated_old_regions < excess_regions) {
      // We'll give only unaffiliated old to young, which is known to be less than the excess.
      old_region_surplus = unaffiliated_old_regions;
    } else {
      // unaffiliated_old_regions > excess_regions, so we only give away the excess.
      old_region_surplus = excess_regions;
    }
  } else {
    // We need to request transfer from YOUNG.  Ignore that this will directly impact young_generation()->max_capacity(),
    // indirectly impacting young_reserve and old_reserve.  These computations are conservative.
    size_t old_need = old_reserve - old_available;
    // Round up the number of regions needed from YOUNG
    old_region_deficit = (old_need + region_size_bytes - 1) / region_size_bytes;
  }
  if (old_region_deficit > max_old_region_xfer) {
    // If we're running short on young-gen memory, limit the xfer.  Old-gen collection activities will be curtailed
    // if the budget is smaller than desired.
    old_region_deficit = max_old_region_xfer;
  }
  set_old_region_surplus(old_region_surplus);
  set_old_region_deficit(old_region_deficit);
}


void ShenandoahGenerationalHeap::prepare_regions_and_collection_set(bool concurrent, ShenandoahGeneration* generation) {
  assert(mode()->is_generational(), "Error");
  assert(!is_full_gc_in_progress(), "Only for concurrent and degenerated GC");
  assert(!generation->is_old(), "Only YOUNG and GLOBAL GC perform evacuations");
  {
    ShenandoahGCPhase phase(concurrent ? ShenandoahPhaseTimings::final_update_region_states :
                            ShenandoahPhaseTimings::degen_gc_final_update_region_states);
    ShenandoahFinalMarkUpdateRegionStateClosure cl(complete_marking_context());
    parallel_heap_region_iterate(&cl);

    if (generation->is_young()) {
      // We always need to update the watermark for old regions. If there
      // are mixed collections pending, we also need to synchronize the
      // pinned status for old regions. Since we are already visiting every
      // old region here, go ahead and sync the pin status too.
      ShenandoahFinalMarkUpdateRegionStateClosure old_cl(nullptr);
      old_generation()->parallel_heap_region_iterate(&old_cl);
    }
  }

  // Tally the census counts and compute the adaptive tenuring threshold
  if (ShenandoahGenerationalAdaptiveTenuring && !ShenandoahGenerationalCensusAtEvac) {
    // Objects above TAMS weren't included in the age census. Since they were all
    // allocated in this cycle they belong in the age 0 cohort. We walk over all
    // young regions and sum the volume of objects between TAMS and top.
    ShenandoahUpdateCensusZeroCohortClosure age0_cl(complete_marking_context());
    young_generation()->heap_region_iterate(&age0_cl);
    size_t age0_pop = age0_cl.get_population();

    // Age table updates
    ShenandoahAgeCensus* census = age_census();
    census->prepare_for_census_update();
    // Update the global census, including the missed age 0 cohort above,
    // along with the census during marking, and compute the tenuring threshold
    census->update_census(age0_pop);
  }

  {
    ShenandoahGCPhase phase(concurrent ? ShenandoahPhaseTimings::choose_cset :
                            ShenandoahPhaseTimings::degen_gc_choose_cset);

    collection_set()->clear();
    ShenandoahHeapLocker locker(lock());

    size_t consumed_by_advance_promotion;
    bool* preselected_regions = (bool*) alloca(num_regions() * sizeof(bool));
    for (unsigned int i = 0; i < num_regions(); i++) {
      preselected_regions[i] = false;
    }

    // TODO: young_available can include available (between top() and end()) within each young region that is not
    // part of the collection set.  Making this memory available to the young_evacuation_reserve allows a larger
    // young collection set to be chosen when available memory is under extreme pressure.  Implementing this "improvement"
    // is tricky, because the incremental construction of the collection set actually changes the amount of memory
    // available to hold evacuated young-gen objects.  As currently implemented, the memory that is available within
    // non-empty regions that are not selected as part of the collection set can be allocated by the mutator while
    // GC is evacuating and updating references.

    // Budgeting parameters to compute_evacuation_budgets are passed by reference.
    compute_evacuation_budgets(generation, preselected_regions, consumed_by_advance_promotion);
    generation->heuristics()->choose_collection_set(collection_set());
    if (!collection_set()->is_empty()) {
      // only make use of evacuation budgets when we are evacuating
      adjust_evacuation_budgets();
    }

    if (generation->is_global()) {
      // We have just chosen a collection set for a global cycle. The mark bitmap covering old regions is complete, so
      // the remembered set scan can use that to avoid walking into garbage. When the next old mark begins, we will
      // use the mark bitmap to make the old regions parsable by coalescing and filling any unmarked objects. Thus,
      // we prepare for old collections by remembering which regions are old at this time. Note that any objects
      // promoted into old regions will be above TAMS, and so will be considered marked. However, free regions that
      // become old after this point will not be covered correctly by the mark bitmap, so we must be careful not to
      // coalesce those regions. Only the old regions which are not part of the collection set at this point are
      // eligible for coalescing. As implemented now, this has the side effect of possibly initiating mixed-evacuations
      // after a global cycle for old regions that were not included in this collection set.
      assert(old_generation()->is_mark_complete(), "Expected old generation mark to be complete after global cycle.");
      old_heuristics()->prepare_for_old_collections();
      log_info(gc)("After choosing global collection set, mixed candidates: " UINT32_FORMAT ", coalescing candidates: " SIZE_FORMAT,
                   old_heuristics()->unprocessed_old_collection_candidates(),
                   old_heuristics()->coalesce_and_fill_candidates_count());
    }
  }

  // Freeset construction uses reserve quantities if they are valid
  set_evacuation_reserve_quantities(true);
  {
    ShenandoahGCPhase phase(concurrent ? ShenandoahPhaseTimings::final_rebuild_freeset :
                            ShenandoahPhaseTimings::degen_gc_final_rebuild_freeset);
    ShenandoahHeapLocker locker(lock());
    size_t young_cset_regions, old_cset_regions;

    // We are preparing for evacuation.  At this time, we ignore cset region tallies.
    free_set()->prepare_to_rebuild(young_cset_regions, old_cset_regions);
    free_set()->rebuild(young_cset_regions, old_cset_regions);
  }
  set_evacuation_reserve_quantities(false);
}


void ShenandoahGenerationalHeap::prepare_regions_and_collection_set_old(bool concurrent, ShenandoahGeneration* generation) {
  assert(mode()->is_generational(), "Error");
  assert(!is_full_gc_in_progress(), "Only for concurrent and degenerated GC");
  assert(generation->is_old(), "Error");

  {
    ShenandoahGCPhase phase(concurrent ?
        ShenandoahPhaseTimings::final_update_region_states :
        ShenandoahPhaseTimings::degen_gc_final_update_region_states);
    ShenandoahFinalMarkUpdateRegionStateClosure cl(generation->complete_marking_context());

    parallel_heap_region_iterate(&cl);
    assert_pinned_region_status();
  }

  {
    // This doesn't actually choose a collection set, but prepares a list of
    // regions as 'candidates' for inclusion in a mixed collection.
    ShenandoahGCPhase phase(concurrent ?
        ShenandoahPhaseTimings::choose_cset :
        ShenandoahPhaseTimings::degen_gc_choose_cset);
    ShenandoahHeapLocker locker(lock());
    old_heuristics()->prepare_for_old_collections();
  }

  {
    // Though we did not choose a collection set above, we still may have
    // freed up immediate garbage regions so proceed with rebuilding the free set.
    ShenandoahGCPhase phase(concurrent ?
        ShenandoahPhaseTimings::final_rebuild_freeset :
        ShenandoahPhaseTimings::degen_gc_final_rebuild_freeset);
    ShenandoahHeapLocker locker(lock());
    size_t cset_young_regions, cset_old_regions;
    free_set()->prepare_to_rebuild(cset_young_regions, cset_old_regions);
    // This is just old-gen completion.  No future budgeting required here.  The only reason to rebuild the freeset here
    // is in case there was any immediate old garbage identified.
    free_set()->rebuild(cset_young_regions, cset_old_regions);
  }
}

void ShenandoahGenerationalHeap::compute_evacuation_budgets(ShenandoahGeneration* generation,
                                                            bool* preselected_regions,
                                                            size_t &consumed_by_advance_promotion) {
  size_t region_size_bytes = ShenandoahHeapRegion::region_size_bytes();
  size_t regions_available_to_loan = 0;
  size_t minimum_evacuation_reserve = ShenandoahOldCompactionReserve * region_size_bytes;
  size_t old_regions_loaned_for_young_evac = 0;
  consumed_by_advance_promotion = 0;

  size_t old_evacuation_reserve = 0;

  // During initialization and phase changes, it is more likely that fewer objects die young and old-gen
  // memory is not yet full (or is in the process of being replaced).  During these times especially, it
  // is beneficial to loan memory from old-gen to young-gen during the evacuation and update-refs phases
  // of execution.

  // Calculate EvacuationReserve before PromotionReserve.  Evacuation is more critical than promotion.
  // If we cannot evacuate old-gen, we will not be able to reclaim old-gen memory.  Promotions are less
  // critical.  If we cannot promote, there may be degradation of young-gen memory because old objects
  // accumulate there until they can be promoted.  This increases the young-gen marking and evacuation work.

  // Do not fill up old-gen memory with promotions.  Reserve some amount of memory for compaction purposes.
  size_t young_evac_reserve_max = 0;

  // First priority is to reclaim the easy garbage out of young-gen.

  // maximum_young_evacuation_reserve is upper bound on memory to be evacuated out of young
  size_t maximum_young_evacuation_reserve = (young_generation()->max_capacity() * ShenandoahEvacReserve) / 100;
  size_t young_evacuation_reserve = maximum_young_evacuation_reserve;
  size_t excess_young;
  if (young_generation()->available() > young_evacuation_reserve) {
    excess_young = young_generation()->available() - young_evacuation_reserve;
  } else {
    young_evacuation_reserve = young_generation()->available();
    excess_young = 0;
  }
  size_t unaffiliated_young = young_generation()->free_unaffiliated_regions() * region_size_bytes;
  if (excess_young > unaffiliated_young) {
    excess_young = unaffiliated_young;
  } else {
    // round down to multiple of region size
    excess_young /= region_size_bytes;
    excess_young *= region_size_bytes;
  }
  // excess_young is available to be transferred to OLD.  Assume that OLD will not request any more than had
  // already been set aside for its promotion and evacuation needs at the end of previous GC.  No need to
  // hold back memory for allocation runway.

  // maximum_old_evacuation_reserve is an upper bound on memory evacuated from old and evacuated to old (promoted).
  size_t maximum_old_evacuation_reserve =
    maximum_young_evacuation_reserve * ShenandoahOldEvacRatioPercent / (100 - ShenandoahOldEvacRatioPercent);
  // Here's the algebra:
  //  TotalEvacuation = OldEvacuation + YoungEvacuation
  //  OldEvacuation = TotalEvacuation * (ShenandoahOldEvacRatioPercent/100)
  //  OldEvacuation = YoungEvacuation * (ShenandoahOldEvacRatioPercent/100)/(1 - ShenandoahOldEvacRatioPercent/100)
  //  OldEvacuation = YoungEvacuation * ShenandoahOldEvacRatioPercent/(100 - ShenandoahOldEvacRatioPercent)

  if (maximum_old_evacuation_reserve > old_generation()->available()) {
    maximum_old_evacuation_reserve = old_generation()->available();
  }

  // Second priority is to reclaim garbage out of old-gen if there are old-gen collection candidates.  Third priority
  // is to promote as much as we have room to promote.  However, if old-gen memory is in short supply, this means young
  // GC is operating under "duress" and was unable to transfer the memory that we would normally expect.  In this case,
  // old-gen will refrain from compacting itself in order to allow a quicker young-gen cycle (by avoiding the update-refs
  // through ALL of old-gen).  If there is some memory available in old-gen, we will use this for promotions as promotions
  // do not add to the update-refs burden of GC.

  size_t old_promo_reserve;
  if (generation->is_global()) {
    // Global GC is typically triggered by user invocation of System.gc(), and typically indicates that there is lots
    // of garbage to be reclaimed because we are starting a new phase of execution.  Marking for global GC may take
    // significantly longer than typical young marking because we must mark through all old objects.  To expedite
    // evacuation and update-refs, we give emphasis to reclaiming garbage first, wherever that garbage is found.
    // Global GC will adjust generation sizes to accommodate the collection set it chooses.

    // Set old_promo_reserve to enforce that no regions are preselected for promotion.  Such regions typically
    // have relatively high memory utilization.  We still call select_aged_regions() because this will prepare for
    // promotions in place, if relevant.
    old_promo_reserve = 0;

    // Dedicate all available old memory to old_evacuation reserve.  This may be small, because old-gen is only
    // expanded based on an existing mixed evacuation workload at the end of the previous GC cycle.  We'll expand
    // the budget for evacuation of old during GLOBAL cset selection.
    old_evacuation_reserve = maximum_old_evacuation_reserve;
  } else if (old_heuristics()->unprocessed_old_collection_candidates() > 0) {
    // We reserved all old-gen memory at end of previous GC to hold anticipated evacuations to old-gen.  If this is
    // mixed evacuation, reserve all of this memory for compaction of old-gen and do not promote.  Prioritize compaction
    // over promotion in order to defragment OLD so that it will be better prepared to efficiently receive promoted memory.
    old_evacuation_reserve = maximum_old_evacuation_reserve;
    old_promo_reserve = 0;
  } else {
    // Make all old-evacuation memory for promotion, but if we can't use it all for promotion, we'll allow some evacuation.
    old_evacuation_reserve = 0;
    old_promo_reserve = maximum_old_evacuation_reserve;
  }

  // We see too many old-evacuation failures if we force ourselves to evacuate into regions that are not initially empty.
  // So we limit the old-evacuation reserve to unfragmented memory.  Even so, old-evacuation is free to fill in nooks and
  // crannies within existing partially used regions and it generally tries to do so.
  size_t old_free_regions = old_generation()->free_unaffiliated_regions();
  size_t old_free_unfragmented = old_free_regions * region_size_bytes;
  if (old_evacuation_reserve > old_free_unfragmented) {
    size_t delta = old_evacuation_reserve - old_free_unfragmented;
    old_evacuation_reserve -= delta;

    // Let promo consume fragments of old-gen memory if not global
    if (!generation->is_global()) {
      old_promo_reserve += delta;
    }
  }
  collection_set()->establish_preselected(preselected_regions);
  consumed_by_advance_promotion = select_aged_regions(old_promo_reserve, num_regions(), preselected_regions);
  assert(consumed_by_advance_promotion <= maximum_old_evacuation_reserve, "Cannot promote more than available old-gen memory");

  // Note that unused old_promo_reserve might not be entirely consumed_by_advance_promotion.  Do not transfer this
  // to old_evacuation_reserve because this memory is likely very fragmented, and we do not want to increase the likelihood
  // of old evacuatino failure.

  set_young_evac_reserve(young_evacuation_reserve);
  set_old_evac_reserve(old_evacuation_reserve);
  set_promoted_reserve(consumed_by_advance_promotion);

  // There is no need to expand OLD because all memory used here was set aside at end of previous GC, except in the
  // case of a GLOBAL gc.  During choose_collection_set() of GLOBAL, old will be expanded on demand.
}

// Having chosen the collection set, adjust the budgets for generational mode based on its composition.  Note
// that young_generation()->available() now knows about recently discovered immediate garbage.

void ShenandoahGenerationalHeap::adjust_evacuation_budgets() {
  // We may find that old_evacuation_reserve and/or loaned_for_young_evacuation are not fully consumed, in which case we may
  //  be able to increase regions_available_to_loan

  // The role of adjust_evacuation_budgets() is to compute the correct value of regions_available_to_loan and to make
  // effective use of this memory, including the remnant memory within these regions that may result from rounding loan to
  // integral number of regions.  Excess memory that is available to be loaned is applied to an allocation supplement,
  // which allows mutators to allocate memory beyond the current capacity of young-gen on the promise that the loan
  // will be repaid as soon as we finish updating references for the recently evacuated collection set.

  // We cannot recalculate regions_available_to_loan by simply dividing old_generation()->available() by region_size_bytes
  // because the available memory may be distributed between many partially occupied regions that are already holding old-gen
  // objects.  Memory in partially occupied regions is not "available" to be loaned.  Note that an increase in old-gen
  // available that results from a decrease in memory consumed by old evacuation is not necessarily available to be loaned
  // to young-gen.

  size_t region_size_bytes = ShenandoahHeapRegion::region_size_bytes();

  // Preselected regions have been inserted into the collection set, so we no longer need the preselected array.
  collection_set()->abandon_preselected();

  size_t old_evacuated = collection_set()->get_old_bytes_reserved_for_evacuation();
  size_t old_evacuated_committed = (size_t) (ShenandoahOldEvacWaste * old_evacuated);
  size_t old_evacuation_reserve = get_old_evac_reserve();

  if (old_evacuated_committed > old_evacuation_reserve) {
    // This should only happen due to round-off errors when enforcing ShenandoahOldEvacWaste
    assert(old_evacuated_committed <= (33 * old_evacuation_reserve) / 32,
           "Round-off errors should be less than 3.125%%, committed: " SIZE_FORMAT ", reserved: " SIZE_FORMAT,
           old_evacuated_committed, old_evacuation_reserve);
    old_evacuated_committed = old_evacuation_reserve;
    // Leave old_evac_reserve as previously configured
  } else if (old_evacuated_committed < old_evacuation_reserve) {
    // This happens if the old-gen collection consumes less than full budget.
    old_evacuation_reserve = old_evacuated_committed;
    set_old_evac_reserve(old_evacuation_reserve);
  }

  size_t young_advance_promoted = collection_set()->get_young_bytes_to_be_promoted();
  size_t young_advance_promoted_reserve_used = (size_t) (ShenandoahPromoEvacWaste * young_advance_promoted);

  size_t young_evacuated = collection_set()->get_young_bytes_reserved_for_evacuation();
  size_t young_evacuated_reserve_used = (size_t) (ShenandoahEvacWaste * young_evacuated);

  assert(young_evacuated_reserve_used <= young_generation()->available(), "Cannot evacuate more than is available in young");
  set_young_evac_reserve(young_evacuated_reserve_used);

  size_t old_available = old_generation()->available();
  // Now that we've established the collection set, we know how much memory is really required by old-gen for evacuation
  // and promotion reserves.  Try shrinking OLD now in case that gives us a bit more runway for mutator allocations during
  // evac and update phases.
  size_t old_consumed = old_evacuated_committed + young_advance_promoted_reserve_used;

  if (old_available < old_consumed) {
    // This can happen due to round-off errors when adding the results of truncated integer arithmetic.
    // We've already truncated old_evacuated_committed.  Truncate young_advance_promoted_reserve_used here.
    assert(young_advance_promoted_reserve_used <= (33 * (old_available - old_evacuated_committed)) / 32,
           "Round-off errors should be less than 3.125%%, committed: " SIZE_FORMAT ", reserved: " SIZE_FORMAT,
           young_advance_promoted_reserve_used, old_available - old_evacuated_committed);
    young_advance_promoted_reserve_used = old_available - old_evacuated_committed;
    old_consumed = old_evacuated_committed + young_advance_promoted_reserve_used;
  }

  assert(old_available >= old_consumed, "Cannot consume (" SIZE_FORMAT ") more than is available (" SIZE_FORMAT ")",
         old_consumed, old_available);
  size_t excess_old = old_available - old_consumed;
  size_t unaffiliated_old_regions = old_generation()->free_unaffiliated_regions();
  size_t unaffiliated_old = unaffiliated_old_regions * region_size_bytes;
  assert(old_available >= unaffiliated_old, "Unaffiliated old is a subset of old available");

  // Make sure old_evac_committed is unaffiliated
  if (old_evacuated_committed > 0) {
    if (unaffiliated_old > old_evacuated_committed) {
      size_t giveaway = unaffiliated_old - old_evacuated_committed;
      size_t giveaway_regions = giveaway / region_size_bytes;  // round down
      if (giveaway_regions > 0) {
        excess_old = MIN2(excess_old, giveaway_regions * region_size_bytes);
      } else {
        excess_old = 0;
      }
    } else {
      excess_old = 0;
    }
  }

  // If we find that OLD has excess regions, give them back to YOUNG now to reduce likelihood we run out of allocation
  // runway during evacuation and update-refs.
  size_t regions_to_xfer = 0;
  if (excess_old > unaffiliated_old) {
    // we can give back unaffiliated_old (all of unaffiliated is excess)
    if (unaffiliated_old_regions > 0) {
      regions_to_xfer = unaffiliated_old_regions;
    }
  } else if (unaffiliated_old_regions > 0) {
    // excess_old < unaffiliated old: we can give back MIN(excess_old/region_size_bytes, unaffiliated_old_regions)
    size_t excess_regions = excess_old / region_size_bytes;
    size_t regions_to_xfer = MIN2(excess_regions, unaffiliated_old_regions);
  }

  if (regions_to_xfer > 0) {
    bool result = generation_sizer()->transfer_to_young(regions_to_xfer);
    assert(excess_old > regions_to_xfer * region_size_bytes, "Cannot xfer more than excess old");
    excess_old -= regions_to_xfer * region_size_bytes;
    log_info(gc, ergo)("%s transferred " SIZE_FORMAT " excess regions to young before start of evacuation",
                       result? "Successfully": "Unsuccessfully", regions_to_xfer);
  }

  // Add in the excess_old memory to hold unanticipated promotions, if any.  If there are more unanticipated
  // promotions than fit in reserved memory, they will be deferred until a future GC pass.
  size_t total_promotion_reserve = young_advance_promoted_reserve_used + excess_old;
  set_promoted_reserve(total_promotion_reserve);
  reset_promoted_expended();
}


// Helper struct & method for sorting used in select_aged_regions() further below
typedef struct {
  ShenandoahHeapRegion* _region;
  size_t _live_data;
} AgedRegionData;

static int compare_by_aged_live(AgedRegionData a, AgedRegionData b) {
  if (a._live_data < b._live_data)
    return -1;
  else if (a._live_data > b._live_data)
    return 1;
  else return 0;
}

#ifdef ASSERT
void ShenandoahGenerationalHeap::assert_no_in_place_promotions() {
  class ShenandoahNoInPlacePromotions : public ShenandoahHeapRegionClosure {
  public:
    void heap_region_do(ShenandoahHeapRegion *r) override {
      assert(r->get_top_before_promote() == nullptr,
             "Region " SIZE_FORMAT " should not be ready for in-place promotion", r->index());
    }
  } cl;
  heap_region_iterate(&cl);
}
#endif

// Preselect for inclusion into the collection set regions whose age is at or above tenure age which contain more than
// ShenandoahOldGarbageThreshold amounts of garbage.  We identify these regions by setting the appropriate entry of
// candidate_regions_for_promotion_by_copy[] to true.  All entries are initialized to false before calling this
// function.
//
// During the subsequent selection of the collection set, we give priority to these promotion set candidates.
// Without this prioritization, we found that the aged regions tend to be ignored because they typically have
// much less garbage and much more live data than the recently allocated "eden" regions.  When aged regions are
// repeatedly excluded from the collection set, the amount of live memory within the young generation tends to
// accumulate and this has the undesirable side effect of causing young-generation collections to require much more
// CPU and wall-clock time.
//
// A second benefit of treating aged regions differently than other regions during collection set selection is
// that this allows us to more accurately budget memory to hold the results of evacuation.  Memory for evacuation
// of aged regions must be reserved in the old generations.  Memory for evacuation of all other regions must be
// reserved in the young generation.
size_t ShenandoahGenerationalHeap::select_aged_regions(size_t old_available, size_t num_regions,
                                                       bool candidate_regions_for_promotion_by_copy[]) {

  // There should be no regions configured for subsequent in-place-promotions carried over from the previous cycle.
  assert_no_in_place_promotions();
  assert(mode()->is_generational(), "Only in generational mode");

  const uint tenuring_threshold = age_census()->tenuring_threshold();

  size_t old_consumed = 0;
  size_t promo_potential = 0;
  size_t anticipated_promote_in_place_live = 0;

  clear_promotion_in_place_potential();
  clear_promotion_potential();

  size_t candidates = 0;
  size_t candidates_live = 0;
  size_t old_garbage_threshold = (ShenandoahHeapRegion::region_size_bytes() * ShenandoahOldGarbageThreshold) / 100;
  size_t promote_in_place_regions = 0;
  size_t promote_in_place_live = 0;
  size_t promote_in_place_pad = 0;
  size_t anticipated_candidates = 0;
  size_t anticipated_promote_in_place_regions = 0;

  // Sort the promotion-eligible regions according to live-data-bytes so that we can first reclaim regions that require
  // less evacuation effort.  This prioritizes garbage first, expanding the allocation pool before we begin the work of
  // reclaiming regions that require more effort.
  AgedRegionData* sorted_regions = (AgedRegionData*) alloca(num_regions * sizeof(AgedRegionData));
  ShenandoahMarkingContext* const ctx = marking_context();
  for (size_t i = 0; i < num_regions; i++) {
    ShenandoahHeapRegion* r = get_region(i);
    if (r->is_empty() || !r->has_live() || !r->is_young() || !r->is_regular()) {
      continue;
    }
    if (r->age() >= tenuring_threshold) {
      if ((r->garbage() < old_garbage_threshold)) {
        HeapWord* tams = ctx->top_at_mark_start(r);
        HeapWord* original_top = r->top();
        if (tams == original_top) {
          // No allocations from this region have been made during concurrent mark. It meets all the criteria
          // for in-place-promotion. Though we only need the value of top when we fill the end of the region,
          // we use this field to indicate that this region should be promoted in place during the evacuation
          // phase.
          r->save_top_before_promote();

          size_t remnant_size = r->free() / HeapWordSize;
          if (remnant_size > ShenandoahHeap::min_fill_size()) {
            ShenandoahHeap::fill_with_object(original_top, remnant_size);
            // Fill the remnant memory within this region to assure no allocations prior to promote in place.  Otherwise,
            // newly allocated objects will not be parsable when promote in place tries to register them.  Furthermore, any
            // new allocations would not necessarily be eligible for promotion.  This addresses both issues.
            r->set_top(r->end());
            promote_in_place_pad += remnant_size * HeapWordSize;
          } else {
            // Since the remnant is so small that it cannot be filled, we don't have to worry about any accidental
            // allocations occurring within this region before the region is promoted in place.
          }
          promote_in_place_regions++;
          promote_in_place_live += r->get_live_data_bytes();
        }
        // Else, we do not promote this region (either in place or by copy) because it has received new allocations.

        // During evacuation, we exclude from promotion regions for which age > tenure threshold, garbage < garbage-threshold,
        //  and get_top_before_promote() != tams
      } else {
        // After sorting and selecting best candidates below, we may decide to exclude this promotion-eligible region
        // from the current collection sets.  If this happens, we will consider this region as part of the anticipated
        // promotion potential for the next GC pass.
        size_t live_data = r->get_live_data_bytes();
        candidates_live += live_data;
        sorted_regions[candidates]._region = r;
        sorted_regions[candidates++]._live_data = live_data;
      }
    } else {
      // We only anticipate to promote regular regions if garbage() is above threshold.  Tenure-aged regions with less
      // garbage are promoted in place.  These take a different path to old-gen.  Note that certain regions that are
      // excluded from anticipated promotion because their garbage content is too low (causing us to anticipate that
      // the region would be promoted in place) may be eligible for evacuation promotion by the time promotion takes
      // place during a subsequent GC pass because more garbage is found within the region between now and then.  This
      // should not happen if we are properly adapting the tenure age.  The theory behind adaptive tenuring threshold
      // is to choose the youngest age that demonstrates no "significant" futher loss of population since the previous
      // age.  If not this, we expect the tenure age to demonstrate linear population decay for at least two population
      // samples, whereas we expect to observe exponetial population decay for ages younger than the tenure age.
      //
      // In the case that certain regions which were anticipated to be promoted in place need to be promoted by
      // evacuation, it may be the case that there is not sufficient reserve within old-gen to hold evacuation of
      // these regions.  The likely outcome is that these regions will not be selected for evacuation or promotion
      // in the current cycle and we will anticipate that they will be promoted in the next cycle.  This will cause
      // us to reserve more old-gen memory so that these objects can be promoted in the subsequent cycle.
      //
      // TODO:
      //   If we are auto-tuning the tenure age and regions that were anticipated to be promoted in place end up
      //   being promoted by evacuation, this event should feed into the tenure-age-selection heuristic so that
      //   the tenure age can be increased.
      if (is_aging_cycle() && (r->age() + 1 == tenuring_threshold)) {
        if (r->garbage() >= old_garbage_threshold) {
          anticipated_candidates++;
          promo_potential += r->get_live_data_bytes();
        }
        else {
          anticipated_promote_in_place_regions++;
          anticipated_promote_in_place_live += r->get_live_data_bytes();
        }
      }
    }
    // Note that we keep going even if one region is excluded from selection.
    // Subsequent regions may be selected if they have smaller live data.
  }
  // Sort in increasing order according to live data bytes.  Note that candidates represents the number of regions
  // that qualify to be promoted by evacuation.
  if (candidates > 0) {
    size_t selected_regions = 0;
    size_t selected_live = 0;
    QuickSort::sort<AgedRegionData>(sorted_regions, candidates, compare_by_aged_live, false);
    for (size_t i = 0; i < candidates; i++) {
      size_t region_live_data = sorted_regions[i]._live_data;
      size_t promotion_need = (size_t) (region_live_data * ShenandoahPromoEvacWaste);
      if (old_consumed + promotion_need <= old_available) {
        ShenandoahHeapRegion* region = sorted_regions[i]._region;
        old_consumed += promotion_need;
        candidate_regions_for_promotion_by_copy[region->index()] = true;
        selected_regions++;
        selected_live += region_live_data;
      } else {
        // We rejected this promotable region from the collection set because we had no room to hold its copy.
        // Add this region to promo potential for next GC.
        promo_potential += region_live_data;
      }
      // We keep going even if one region is excluded from selection because we need to accumulate all eligible
      // regions that are not preselected into promo_potential
    }
    log_info(gc)("Preselected " SIZE_FORMAT " regions containing " SIZE_FORMAT " live bytes,"
                 " consuming: " SIZE_FORMAT " of budgeted: " SIZE_FORMAT,
                 selected_regions, selected_live, old_consumed, old_available);
  }
  set_pad_for_promote_in_place(promote_in_place_pad);
  set_promotion_potential(promo_potential);
  set_promotion_in_place_potential(anticipated_promote_in_place_live);
  return old_consumed;
}
