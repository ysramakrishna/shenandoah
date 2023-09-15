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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHGENERATIONALHEAP_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHGENERATIONALHEAP_HPP

#include "gc/shenandoah/shenandoahHeap.hpp"

// Forward declarations
class ShenandoahHeap;

// ShenandoahGenerationalHeap is a ShenandoahHeap with two generations, a younger
// generation in which objects are allocated and an older generation into which
// longer survivors are tenured into. It uses a card-marking barrier to track references
// from the older generation into the younger. Each generation is collected using
// a "copy" of the ShenandoahCollector, with the higher frequency younger generation
// collector interrupting the older generation collector which runs at lower frequency.

class ShenandoahGenerationalHeap : public ShenandoahHeap {
  // Supported GC
  friend class ShenandoahConcurrentGC;
  friend class ShenandoahOldGC;
  friend class ShenandoahDegenGC;
  friend class ShenandoahFullGC;
  friend class ShenandoahUnload;

private:
  // ShenandoahGeneration* _gc_generation;

  // true iff we are concurrently coalescing and filling old-gen HeapRegions
  bool _prepare_for_old_mark;

  // ---------- Heap counters and metrics
  //
  size_t _promotion_potential;
  size_t _promotion_in_place_potential;
  size_t _pad_for_promote_in_place;    // bytes of filler
  size_t _promotable_humongous_regions;
  size_t _promotable_humongous_usage;
  size_t _regular_regions_promoted_in_place;
  size_t _regular_usage_promoted_in_place;


  size_t _promoted_reserve;            // Bytes reserved within old-gen to hold the results of promotion
  volatile size_t _promoted_expended;  // Bytes of old-gen memory expended on promotions

  // Allocation of old GCLABs (aka PLABs) assures that _old_evac_expended + request-size < _old_evac_reserved.  If the allocation
  //  is authorized, increment _old_evac_expended by request size.  This allocation ignores old_gen->available().
  size_t _old_evac_reserve;            // Bytes reserved within old-gen to hold evacuated objects from old-gen collection set
  volatile size_t _old_evac_expended;  // Bytes of old-gen memory expended on old-gen evacuations
  size_t _young_evac_reserve;          // Bytes reserved within young-gen to hold evacuated objects from young-gen collection set
  size_t _captured_old_usage;          // What was old usage (bytes) when last captured?
  size_t _previous_promotion;          // Bytes promoted during previous evacuation
  
  ShenandoahAgeCensus* _age_census;    // Age census used for adapting tenuring threshold in generational mode

  ShenandoahGenerationSizer _generation_sizer;

  ShenandoahYoungGeneration* _young_generation;
  ShenandoahOldGeneration*   _old_generation;
  
  // ---------- VM subsystem bindings
  //
  MemoryPool*                  _young_gen_memory_pool;
  MemoryPool*                  _old_gen_memory_pool;

  // How many bytes to transfer between old and young after we have finished recycling collection set regions?
  size_t _old_regions_surplus;
  size_t _old_regions_deficit;

  // ---------- Class Unloading
  //
  ShenandoahSharedFlag  _is_aging_cycle;


  // ---------- Generational remembered set support
  //
  RememberedScanner* _card_scan;

public:
  bool doing_mixed_evacuations();
  bool is_old_bitmap_stable() const;
  // bool is_gc_generation_young() const;
  
  // ---------- Allocation, including promotion local allocation buffers
  //
  inline HeapWord* allocate_from_plab(Thread* thread, size_t size, bool is_promotion);
private:
  HeapWord* allocate_from_plab_slow(Thread* thread, size_t size, bool is_promotion);
  HeapWord* allocate_new_plab(size_t min_size, size_t word_size, size_t* actual_size);
  

  // ---------- Initialization, termination, identification, printing routines
  //
public:
  // static ShenandoahGenerationalHeap* heap();
  static inline ShenandoahGenerationalHeap* gen_heap();

  const char* name()          const override { return "Shenandoah Generational"; }
  ShenandoahGenerationalHeap::Name kind() const override { return CollectedHeap::ShenandoahGenerational; }

  ShenandoahGenerationalHeap(ShenandoahCollectorPolicy* policy);

  void print_init_logger() const override;

  ShenandoahYoungGeneration* young_generation()  const { return _young_generation;  }
  ShenandoahOldGeneration*   old_generation()    const { return _old_generation;    }
  const ShenandoahGenerationSizer* generation_sizer()  const { return &_generation_sizer;  }

  size_t max_size_for(ShenandoahGeneration* generation) const;
  size_t min_size_for(ShenandoahGeneration* generation) const;

  void initialize_heuristics() override;

  ShenandoahOldHeuristics* old_heuristics();
  ShenandoahYoungHeuristics* young_heuristics();

  inline bool is_in_active_generation(oop obj) const;
  inline bool is_in_young(const void* p) const;
  inline bool is_in_old(const void* p) const;
  inline bool is_old(oop pobj) const;

  void prepare_regions_and_collection_set(bool concurrent, ShenandoahGeneration* generation);
  void prepare_regions_and_collection_set_old(bool concurrent, ShenandoahGeneration* generation);

  inline RememberedScanner* card_scan() { return _card_scan; }
  void clear_cards_for(ShenandoahHeapRegion* region);
  void dirty_cards(HeapWord* start, HeapWord* end);
  void clear_cards(HeapWord* start, HeapWord* end);
  void mark_card_as_dirty(void* location);
  void retire_plab(PLAB* plab);
  void retire_plab(PLAB* plab, Thread* thread);
  void cancel_old_gc();
  bool is_old_gc_active();

  void adjust_generation_sizes_for_next_cycle(size_t old_xfer_limit, size_t young_cset_regions, size_t old_cset_regions);

  void set_young_lab_region_flags();           
  
  inline void set_old_region_surplus(size_t surplus) { _old_regions_surplus = surplus; };
  inline void set_old_region_deficit(size_t deficit) { _old_regions_deficit = deficit; };

  inline size_t get_old_region_surplus() { return _old_regions_surplus; };
  inline size_t get_old_region_deficit() { return _old_regions_deficit; };
  
  inline size_t capture_old_usage(size_t usage);
  inline void set_previous_promotion(size_t promoted_bytes);
  inline size_t get_previous_promotion() const;
  
  inline void clear_promotion_potential() { _promotion_potential = 0; };
  inline void set_promotion_potential(size_t val) { _promotion_potential = val; }; 
  inline size_t get_promotion_potential() { return _promotion_potential; };
  
  inline void clear_promotion_in_place_potential() { _promotion_in_place_potential = 0; };
  inline void set_promotion_in_place_potential(size_t val) { _promotion_in_place_potential = val; };
  inline size_t get_promotion_in_place_potential() { return _promotion_in_place_potential; };
  
  inline void set_pad_for_promote_in_place(size_t pad) { _pad_for_promote_in_place = pad; }
  inline size_t get_pad_for_promote_in_place() { return _pad_for_promote_in_place; }
  
  inline void reserve_promotable_humongous_regions(size_t region_count) { _promotable_humongous_regions = region_count; }
  inline void reserve_promotable_humongous_usage(size_t bytes) { _promotable_humongous_usage = bytes; }
  inline void reserve_promotable_regular_regions(size_t region_count) { _regular_regions_promoted_in_place = region_count; }
  inline void reserve_promotable_regular_usage(size_t used_bytes) { _regular_usage_promoted_in_place = used_bytes; }
  
  inline size_t get_promotable_humongous_regions() { return _promotable_humongous_regions; }
  inline size_t get_promotable_humongous_usage() { return _promotable_humongous_usage; }
  inline size_t get_regular_regions_promoted_in_place() { return _regular_regions_promoted_in_place; }
  inline size_t get_regular_usage_promoted_in_place() { return _regular_usage_promoted_in_place; }

  // Returns previous value
  inline size_t set_promoted_reserve(size_t new_val);
  inline size_t get_promoted_reserve() const;
  inline void augment_promo_reserve(size_t increment);

  inline void reset_promoted_expended();
  inline size_t expend_promoted(size_t increment);
  inline size_t unexpend_promoted(size_t decrement);
  inline size_t get_promoted_expended();

  // Returns previous value
  inline size_t set_old_evac_reserve(size_t new_val);
  inline size_t get_old_evac_reserve() const;
  inline void augment_old_evac_reserve(size_t increment);

  inline void reset_old_evac_expended();
  inline size_t expend_old_evac(size_t increment);
  inline size_t get_old_evac_expended();

  // Returns previous value
  inline size_t set_young_evac_reserve(size_t new_val);
  inline size_t get_young_evac_reserve() const;

  inline bool clear_old_evacuation_failure();

  // Return the age census object for young gen (in generational mode)
  inline ShenandoahAgeCensus* age_census() const;

  inline bool is_prepare_for_old_mark_in_progress() const;
  inline bool is_aging_cycle() const;

private:
  // Compute evacuation budgets prior to choosing collection set.
  void compute_evacuation_budgets(ShenandoahGeneration* generation, bool* preselected_regions, size_t& consumed_by_advance_promotion);

  // Adjust evacuation budgets after choosing collection set.
  void adjust_evacuation_budgets();

  // Preselect for inclusion into the collection set regions whose age is
  // at or above tenure age and which contain more than ShenandoahOldGarbageThreshold
  // amounts of garbage.
  //
  // A side effect performed by this function is to tally up the number of regions and
  // the number of live bytes that we plan to promote-in-place during the current GC cycle.
  // This information, which is stored with an invocation of heap->set_promotion_in_place_potential(),
  // feeds into subsequent decisions about when to trigger the next GC and may identify
  // special work to be done during this GC cycle if we choose to abbreviate it.
  //
  // Returns bytes of old-gen memory consumed by selected aged regions
  size_t select_aged_regions(size_t old_available, size_t num_regions, bool candidate_regions_for_promotion_by_copy[]);

  void assert_no_in_place_promotions() PRODUCT_RETURN;

  ShenandoahSharedFlag _old_gen_oom_evac;


public:
  void handle_old_evacuation(HeapWord* obj, size_t words, bool promotion);
  void handle_old_evacuation_failure();
  void report_promotion_failure(Thread* thread, size_t size);
};

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHGENERATIONALHEAP_HPP
