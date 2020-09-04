/*
 * This file is about the fast test of in-cset or not 
 * The Regions in the CSet are not maintained by the classes in current file.
 * 
 */

#ifndef SHARE_VM_GC_G1_SEMERU_G1INCSETSTATE_HPP
#define SHARE_VM_GC_G1_SEMERU_G1INCSETSTATE_HPP

#include "gc/g1/g1BiasedArray.hpp"
#include "gc/g1/SemeruHeapRegion.hpp"

// Per-region state during garbage collection.
// The value for the CSet fast-test array
struct SemeruInCSetState {
 public:
  // We use different types to represent the state value. Particularly SPARC puts
  // values in structs from "left to right", i.e. MSB to LSB. This results in many
  // unnecessary shift operations when loading and storing values of this type.
  // This degrades performance significantly (>10%) on that platform.
  // Other tested ABIs do not seem to have this problem, and actually tend to
  // favor smaller types, so we use the smallest usable type there.
#ifdef SPARC
  #define CSETSTATE_FORMAT INTPTR_FORMAT
  typedef intptr_t in_cset_state_t;
#else
  #define CSETSTATE_FORMAT "%d"
  typedef int8_t in_cset_state_t;
#endif
 private:
  in_cset_state_t _value;
 public:
  enum {
    // Selection of the values were driven to micro-optimize the encoding and
    // frequency of the checks.
    // The most common check is whether the region is in the collection set or not,
    // this encoding allows us to use an > 0 check.
    // The positive values are encoded in increasing generation order, which
    // makes getting the next generation fast by a simple increment. They are also
    // used to index into arrays.
    // The negative values are used for objects requiring various special cases,
    // for example eager reclamation of humongous objects or optional regions.
    Optional     = -2,    // The region is optional
    Humongous    = -1,    // The region is humongous
    NotInCSet    =  0,    // The region is not in the collection set.
    FreshlyEvict =  1,    // The region is in the freshly evicted collection set.
    Scanned      =  2,    // The region is in the concurrent scanned collection set.
    Num
  };

  SemeruInCSetState(in_cset_state_t value = NotInCSet) : _value(value) {
    assert(is_valid(), "Invalid state %d", _value);
  }

  in_cset_state_t value() const        { return _value; }

  void set_scanned()                   { _value = Scanned; }
  void set_freslhy_evict()             {  _value = FreshlyEvict;  }

  bool is_in_cset_or_humongous() const { return is_in_cset() || is_humongous(); }
  bool is_in_cset() const              { return _value > NotInCSet; }   // Could be in both freshly_evict or scanned CSet.

  bool is_humongous() const            { return _value == Humongous; }
  bool is_fresly_evict() const         { return _value == FreshlyEvict; }
  bool is_scanned() const               { return _value == Scanned; } 
  bool is_optional() const             { return _value == Optional; }     // [?] meaning of this ? the old region in CSet ?

#ifdef ASSERT
  bool is_default() const              { return _value == NotInCSet; }
  bool is_valid() const                { return (_value >= Optional) && (_value < Num); }
  bool is_valid_gen() const            { return (_value >= FreshlyEvict && _value <= Scanned); }
#endif
};


// Fast-test
// Instances of this class are used for quick tests on whether a reference points
// into the collection set and into which generation or is a humongous object
//
// Each of the array's elements indicates whether the corresponding region is in
// the collection set and if so in which generation, or a humongous region.
//
// We use this to speed up reference processing during young collection and
// quickly reclaim humongous objects. For the latter, by making a humongous region
// succeed this test, we sort-of add it to the collection set. During the reference
// iteration closures, when we see a humongous region, we then simply mark it as
// referenced, i.e. live.
class G1SemeruInCSetStateFastTestBiasedMappedArray : public G1BiasedMappedArray<SemeruInCSetState> {
 protected:
  SemeruInCSetState default_value() const { return SemeruInCSetState::NotInCSet; }
 public:
  void set_optional(uintptr_t index) {
    assert(get_by_index(index).is_default(),
           "State at index " INTPTR_FORMAT " should be default but is " CSETSTATE_FORMAT, index, get_by_index(index).value());
    set_by_index(index, SemeruInCSetState::Optional);
  }

  void set_humongous(uintptr_t index) {
    assert(get_by_index(index).is_default(),
           "State at index " INTPTR_FORMAT " should be default but is " CSETSTATE_FORMAT, index, get_by_index(index).value());
    set_by_index(index, SemeruInCSetState::Humongous);
  }

  void clear_humongous(uintptr_t index) {
    set_by_index(index, SemeruInCSetState::NotInCSet);
  }

  void set_in_freshly_evict(uintptr_t index) {
    assert(get_by_index(index).is_default(),
           "State at index " INTPTR_FORMAT " should be default but is " CSETSTATE_FORMAT, index, get_by_index(index).value());
    set_by_index(index, SemeruInCSetState::FreshlyEvict);
  }

  void set_in_scanned(uintptr_t index) {
    assert(get_by_index(index).is_default(),
           "State at index " INTPTR_FORMAT " should be default but is " CSETSTATE_FORMAT, index, get_by_index(index).value());
    set_by_index(index, SemeruInCSetState::Scanned);
  }

  bool is_in_cset_or_humongous(HeapWord* addr) const { return at(addr).is_in_cset_or_humongous(); }
  bool is_in_cset(HeapWord* addr) const { return at(addr).is_in_cset(); }
  bool is_in_cset(const SemeruHeapRegion* hr) const { return get_by_index(hr->hrm_index()).is_in_cset(); }
  SemeruInCSetState at(HeapWord* addr) const { return get_by_address(addr); }
  void clear() { G1BiasedMappedArray<SemeruInCSetState>::clear(); }
  void clear(const SemeruHeapRegion* hr) { return set_by_index(hr->hrm_index(), SemeruInCSetState::NotInCSet); }
};

#endif // SHARE_VM_GC_G1_G1INCSETSTATE_HPP
