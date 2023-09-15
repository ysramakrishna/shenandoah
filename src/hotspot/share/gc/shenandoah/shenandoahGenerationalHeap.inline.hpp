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
    
#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHGENERATIONALHEAP_INLINE_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHGENERATIONALHEAP_INLINE_HPP

#include "gc/shenandoah/shenandoahGenerationalHeap.hpp"

#include "gc/shenandoah/mode/shenandoahMode.hpp"
#include "gc/shenandoah/shenandoahThreadLocalData.hpp"
#include "gc/shared/tlab_globals.hpp"

inline ShenandoahGenerationalHeap* ShenandoahGenerationalHeap::gen_heap() {
  return named_heap<ShenandoahGenerationalHeap>(CollectedHeap::Shenandoah);
}

inline bool ShenandoahGenerationalHeap::is_in_active_generation(oop obj) const {
  assert(mode()->is_generational(), "Error");

  if (active_generation() == nullptr) {
    // no collection is happening, only expect this to be called
    // when concurrent processing is active, but that could change
    return false;
  }

  assert(is_in(obj), "only check if is in active generation for objects (" PTR_FORMAT ") in heap", p2i(obj));
  assert((active_generation() == (ShenandoahGeneration*) old_generation()) ||
         (active_generation() == (ShenandoahGeneration*) young_generation()) ||
         (active_generation() == global_generation()), "Active generation must be old, young, or global");

  size_t index = heap_region_containing(obj)->index();
  switch (_affiliations[index]) {
  case ShenandoahAffiliation::FREE:
    // Free regions are in Old, Young, Global
    return true;
  case ShenandoahAffiliation::YOUNG_GENERATION:
    // Young regions are in young_generation and global_generation, not in old_generation
    return (active_generation() != (ShenandoahGeneration*) old_generation());
  case ShenandoahAffiliation::OLD_GENERATION:
    // Old regions are in old_generation and global_generation, not in young_generation
    return (active_generation() != (ShenandoahGeneration*) young_generation());
  default:
    assert(false, "Bad affiliation (%d) for region " SIZE_FORMAT, _affiliations[index], index);
    return false;
  }
}

inline bool ShenandoahGenerationalHeap::is_in_young(const void* p) const {
  return is_in(p) && (_affiliations[heap_region_index_containing(p)] == ShenandoahAffiliation::YOUNG_GENERATION);
}

inline bool ShenandoahGenerationalHeap::is_in_old(const void* p) const {
  return is_in(p) && (_affiliations[heap_region_index_containing(p)] == ShenandoahAffiliation::OLD_GENERATION);
}

inline bool ShenandoahGenerationalHeap::is_old(oop obj) const {
  return is_gc_generation_young() && is_in_old(obj);
}

inline ShenandoahAgeCensus* ShenandoahGenerationalHeap::age_census() const {
  assert(mode()->is_generational(), "Only in generational mode");
  assert(_age_census != nullptr, "Error: not initialized");
  return _age_census;
}

inline bool ShenandoahGenerationalHeap::is_aging_cycle() const {
  return _is_aging_cycle.is_set();
}

inline bool ShenandoahGenerationalHeap::is_prepare_for_old_mark_in_progress() const {
  return _prepare_for_old_mark;
}

inline size_t ShenandoahGenerationalHeap::set_promoted_reserve(size_t new_val) {
  size_t orig = _promoted_reserve;
  _promoted_reserve = new_val;
  return orig;
}

inline size_t ShenandoahGenerationalHeap::get_promoted_reserve() const {
  return _promoted_reserve;
}

// returns previous value
size_t ShenandoahGenerationalHeap::capture_old_usage(size_t old_usage) {
  size_t previous_value = _captured_old_usage;
  _captured_old_usage = old_usage;
  return previous_value;
}

void ShenandoahGenerationalHeap::set_previous_promotion(size_t promoted_bytes) {
  shenandoah_assert_heaplocked();
  _previous_promotion = promoted_bytes;
}

size_t ShenandoahGenerationalHeap::get_previous_promotion() const {
  return _previous_promotion;
}

inline size_t ShenandoahGenerationalHeap::set_old_evac_reserve(size_t new_val) {
  size_t orig = _old_evac_reserve;
  _old_evac_reserve = new_val;
  return orig;
}

inline size_t ShenandoahGenerationalHeap::get_old_evac_reserve() const {
  return _old_evac_reserve;
}

inline void ShenandoahGenerationalHeap::augment_old_evac_reserve(size_t increment) {
  _old_evac_reserve += increment;
}

inline void ShenandoahGenerationalHeap::augment_promo_reserve(size_t increment) {
  _promoted_reserve += increment;
}

inline void ShenandoahGenerationalHeap::reset_old_evac_expended() {
  Atomic::store(&_old_evac_expended, (size_t) 0);
}

inline size_t ShenandoahGenerationalHeap::expend_old_evac(size_t increment) {
  return Atomic::add(&_old_evac_expended, increment);
}

inline size_t ShenandoahGenerationalHeap::get_old_evac_expended() {
  return Atomic::load(&_old_evac_expended);
}

inline void ShenandoahGenerationalHeap::reset_promoted_expended() {
  Atomic::store(&_promoted_expended, (size_t) 0);
}

inline size_t ShenandoahGenerationalHeap::expend_promoted(size_t increment) {
  return Atomic::add(&_promoted_expended, increment);
}

inline size_t ShenandoahGenerationalHeap::unexpend_promoted(size_t decrement) {
  return Atomic::sub(&_promoted_expended, decrement);
}

inline size_t ShenandoahGenerationalHeap::get_promoted_expended() {
  return Atomic::load(&_promoted_expended);
}

inline size_t ShenandoahGenerationalHeap::set_young_evac_reserve(size_t new_val) {
  size_t orig = _young_evac_reserve;
  _young_evac_reserve = new_val;
  return orig;
}

inline size_t ShenandoahGenerationalHeap::get_young_evac_reserve() const {
  return _young_evac_reserve;
}

inline bool ShenandoahGenerationalHeap::clear_old_evacuation_failure() {
  return _old_gen_oom_evac.try_unset();
}

inline void ShenandoahGenerationalHeap::clear_cards_for(ShenandoahHeapRegion* region) {
  assert(mode()->is_generational(), "Error");
  _card_scan->mark_range_as_empty(region->bottom(), pointer_delta(region->end(), region->bottom()));
}

inline void ShenandoahGenerationalHeap::dirty_cards(HeapWord* start, HeapWord* end) {
  assert(mode()->is_generational(), "Should only be used for generational mode");
  size_t words = pointer_delta(end, start);
  _card_scan->mark_range_as_dirty(start, words);
}

inline void ShenandoahGenerationalHeap::clear_cards(HeapWord* start, HeapWord* end) {
  assert(mode()->is_generational(), "Should only be used for generational mode");
  size_t words = pointer_delta(end, start);
  _card_scan->mark_range_as_clean(start, words);
}

inline void ShenandoahGenerationalHeap::mark_card_as_dirty(void* location) {
  assert(mode()->is_generational(), "Error");
  _card_scan->mark_card_as_dirty((HeapWord*)location);
}

inline HeapWord* ShenandoahGenerationalHeap::allocate_from_plab(Thread* thread, size_t size, bool is_promotion) {
  assert(UseTLAB, "TLABs should be enabled");
  
  PLAB* plab = ShenandoahThreadLocalData::plab(thread);
  HeapWord* obj;
    
  if (plab == nullptr) {
    assert(!thread->is_Java_thread() && !thread->is_Worker_thread(), "Performance: thread should have PLAB: %s", thread->name());   
    // No PLABs in this thread, fallback to shared allocation
    return nullptr;
  } else if (is_promotion && !ShenandoahThreadLocalData::allow_plab_promotions(thread)) {
    return nullptr;
  }   
  // if plab->word_size() <= 0, thread's plab not yet initialized for this pass, so allow_plab_promotions() is not trustworthy
  obj = plab->allocate(size);
  if ((obj == nullptr) && (plab->words_remaining() < PLAB::min_size())) {
  // allocate_from_plab_slow will establish allow_plab_promotions(thread) for future invocations
  obj = allocate_from_plab_slow(thread, size, is_promotion);
  }
  // if plab->words_remaining() >= PLAB::min_size(), just return nullptr so we can use a shared allocation
  if (obj == nullptr) {
    return nullptr;
  } 

  if (is_promotion) {
    ShenandoahThreadLocalData::add_to_plab_promoted(thread, size * HeapWordSize);
  } else {
    ShenandoahThreadLocalData::add_to_plab_evacuated(thread, size * HeapWordSize);
  }   
  return obj;
}

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHGENERATIONALHEAP_INLINE_HPP
