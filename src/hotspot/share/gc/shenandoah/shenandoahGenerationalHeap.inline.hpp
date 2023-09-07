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
    
#include "gc/shenandoah/mode/shenandoahMode.hpp"
#include "gc/shenandoah/shenandoahGenerationalHeap.hpp"

inline bool ShenandoahGenerationalHeap::is_in_active_generation(oop obj) const {
  if (!mode()->is_generational()) {
    // everything is the same single generation
    return true;
  }

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

