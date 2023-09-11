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

  ShenandoahYoungGeneration* _young_generation;
  ShenandoahOldGeneration*   _old_generation;
  
  bool doing_mixed_evacuations();
  bool is_old_bitmap_stable() const;
  bool is_gc_generation_young() const;

// ---------- Initialization, termination, identification, printing routines
//
public:
  static inline ShenandoahGenerationalHeap* gen_heap();

  const char* name()          const override { return "Shenandoah Generational"; }
  ShenandoahGenerationalHeap::Name kind() const override { return CollectedHeap::ShenandoahGenerational; }

  // explicit ShenandoahGenerationalHeap(ShenandoahCollectorPolicy* policy) : ShenandoahHeap(policy) {}
  // static ShenandoahGenerationalHeap* heap();

  ShenandoahGenerationalHeap(ShenandoahCollectorPolicy* policy);

  void print_init_logger() const override;

  ShenandoahYoungGeneration* young_generation()  const { return _young_generation;  }
  ShenandoahOldGeneration*   old_generation()    const { return _old_generation;    }

  ShenandoahOldHeuristics* old_heuristics();
  ShenandoahYoungHeuristics* young_heuristics();

  inline bool is_in_active_generation(oop obj) const;
  inline bool is_in_young(const void* p) const;
  inline bool is_in_old(const void* p) const;
  inline bool is_old(oop pobj) const;

  void prepare_regions_and_collection_set(bool concurrent, ShenandoahGeneration* generation);
  void prepare_regions_and_collection_set_old(bool concurrent, ShenandoahGeneration* generation);

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
};

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHGENERATIONALHEAP_HPP
