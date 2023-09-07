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
#include "gc/shenandoah/shenandoahMarkClosures.hpp"
#include "gc/shenandoah/shenandoahOldGeneration.hpp"
#include "gc/shenandoah/shenandoahUtils.hpp"
#include "gc/shenandoah/shenandoahYoungGeneration.hpp"

#include "utilities/quickSort.hpp"

ShenandoahOldHeuristics* ShenandoahGenerationalHeap::old_heuristics() {
  return (ShenandoahOldHeuristics*) _old_generation->heuristics();
}

ShenandoahYoungHeuristics* ShenandoahGenerationalHeap::young_heuristics() {
  return (ShenandoahYoungHeuristics*) _young_generation->heuristics();
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

