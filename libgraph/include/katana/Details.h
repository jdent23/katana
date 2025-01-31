/*
 * This file belongs to the Galois project, a C++ library for exploiting
 * parallelism. The code is being released under the terms of the 3-Clause BSD
 * License (a copy is located in LICENSE.txt at the top-level directory).
 *
 * Copyright (C) 2018, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 */

#ifndef KATANA_LIBGRAPH_KATANA_DETAILS_H_
#define KATANA_LIBGRAPH_KATANA_DETAILS_H_

#include <algorithm>

#include <boost/mpl/if.hpp>

#include "katana/Context.h"
#include "katana/LazyObject.h"
#include "katana/NUMAArray.h"
#include "katana/NoDerefIterator.h"
#include "katana/PerThreadStorage.h"
#include "katana/Range.h"
#include "katana/Threads.h"
#include "katana/config.h"

namespace katana {

struct read_default_graph_tag {};
struct read_with_aux_graph_tag {};
struct read_lc_inout_graph_tag {};
struct read_with_aux_first_graph_tag {};

}  // namespace katana

namespace katana::internal {

template <typename, typename, typename, typename, typename>
struct EdgeSortReference;
}  // namespace katana::internal

namespace katana {

//! Proxy object for internal EdgeSortReference
template <typename GraphNode, typename EdgeTy>
class EdgeSortValue : public StrictObject<EdgeTy> {
  template <typename, typename, typename, typename, typename>
  friend struct internal::EdgeSortReference;

  GraphNode rawDst;

public:
  GraphNode dst;
  typedef StrictObject<EdgeTy> Super;
  typedef typename Super::value_type value_type;

  EdgeSortValue(GraphNode d, GraphNode rd, const value_type& v)
      : Super(v), rawDst(rd), dst(d) {}

  template <typename ER>
  EdgeSortValue(const ER& ref) {
    ref.initialize(*this);
  }
};

}  // namespace katana

namespace katana::internal {

template <bool Enable>
class LocalIteratorFeature {
  typedef std::pair<uint64_t, uint64_t> Range;
  PerThreadStorage<Range> localIterators;

public:
  uint64_t localBegin(uint64_t numNodes) const {
    return std::min(localIterators.getLocal()->first, numNodes);
  }

  uint64_t localEnd(uint64_t numNodes) const {
    return std::min(localIterators.getLocal()->second, numNodes);
  }

  void setLocalRange(uint64_t begin, uint64_t end) {
    Range& r = *localIterators.getLocal();
    r.first = begin;
    r.second = end;
  }
};

template <>
struct LocalIteratorFeature<false> {
  uint64_t localBegin(uint64_t numNodes) const {
    unsigned int id = ThreadPool::getTID();
    unsigned int num = katana::getActiveThreads();
    uint64_t begin = (numNodes + num - 1) / num * id;
    return std::min(begin, numNodes);
  }

  uint64_t localEnd(uint64_t numNodes) const {
    unsigned int id = ThreadPool::getTID();
    unsigned int num = katana::getActiveThreads();
    uint64_t end = (numNodes + num - 1) / num * (id + 1);
    return std::min(end, numNodes);
  }

  void setLocalRange(uint64_t, uint64_t) {}
};

//! Proxy object for {@link EdgeSortIterator}
template <
    typename GraphNode, typename EdgeIndex, typename EdgeDst, typename EdgeData,
    typename GraphNodeConverter>
struct EdgeSortReference {
  typedef typename EdgeData::raw_value_type EdgeTy;
  EdgeIndex at;
  EdgeDst* edgeDst;
  EdgeData* edgeData;

  EdgeSortReference(EdgeIndex x, EdgeDst* dsts, EdgeData* data)
      : at(x), edgeDst(dsts), edgeData(data) {}

  // Explicitly declare what the implicit copy constructor
  // would do since using the implicit copy constructor
  // from a class with a non-defaulted copy assignment
  // operator is deprecated.
  EdgeSortReference(EdgeSortReference const& x) {
    at = x.at;
    edgeDst = x.edgeDst;
    edgeData = x.edgeData;
  }

  EdgeSortReference operator=(const EdgeSortValue<GraphNode, EdgeTy>& x) {
    edgeDst->set(at, x.rawDst);
    edgeData->set(at, x.get());
    return *this;
  }

  EdgeSortReference operator=(const EdgeSortReference& x) {
    edgeDst->set(at, edgeDst->at(x.at));
    edgeData->set(at, edgeData->at(x.at));
    return *this;
  }

  EdgeSortValue<GraphNode, EdgeTy> operator*() const {
    return EdgeSortValue<GraphNode, EdgeTy>(
        GraphNodeConverter()(edgeDst->at(at)), edgeDst->at(at),
        edgeData->at(at));
  }

  void initialize(EdgeSortValue<GraphNode, EdgeTy>& value) const {
    value = *(*this);
  }
};

/**
 * Converts comparison functions over EdgeTy to be over {@link EdgeSortValue}.
 */
template <typename EdgeSortValueTy, typename CompTy>
struct EdgeSortCompWrapper {
  const CompTy& comp;

  EdgeSortCompWrapper(const CompTy& c) : comp(c) {}
  bool operator()(const EdgeSortValueTy& a, const EdgeSortValueTy& b) const {
    return comp(a.get(), b.get());
  }
};

struct Identity {
  template <typename T>
  T operator()(const T& x) const {
    return x;
  }
};

/**
 * Iterator to facilitate sorting of CSR-like graphs. Converts random access
 * operations on iterator to appropriate computations on edge destinations and
 * edge data.
 *
 * @tparam GraphNode Graph node pointer
 * @tparam EdgeIndex Integer-like value that is passed to EdgeDst and EdgeData
 * @tparam EdgeDst {@link NUMAArray}-like container of edge destinations
 * @tparam EdgeData {@link NUMAArray}-like container of edge data
 * @tparam GraphNodeConverter A functor to apply when returning values of
 *   EdgeDst when dereferencing this iterator; assignment uses untransformed
 *   EdgeDst values
 */
template <
    typename GraphNode, typename EdgeIndex, typename EdgeDst, typename EdgeData,
    typename GraphNodeConverter = Identity>
class EdgeSortIterator
    : public boost::iterator_facade<
          EdgeSortIterator<
              GraphNode, EdgeIndex, EdgeDst, EdgeData, GraphNodeConverter>,
          EdgeSortValue<GraphNode, typename EdgeData::raw_value_type>,
          boost::random_access_traversal_tag,
          EdgeSortReference<
              GraphNode, EdgeIndex, EdgeDst, EdgeData, GraphNodeConverter>> {
  typedef EdgeSortIterator<
      GraphNode, EdgeIndex, EdgeDst, EdgeData, GraphNodeConverter>
      Self;
  typedef EdgeSortReference<
      GraphNode, EdgeIndex, EdgeDst, EdgeData, GraphNodeConverter>
      Reference;

  EdgeIndex at;
  EdgeDst* edgeDst;
  EdgeData* edgeData;

public:
  EdgeSortIterator() : at(0) {}
  EdgeSortIterator(EdgeIndex x, EdgeDst* dsts, EdgeData* data)
      : at(x), edgeDst(dsts), edgeData(data) {}

private:
  friend class boost::iterator_core_access;

  bool equal(const Self& other) const { return at == other.at; }
  Reference dereference() const { return Reference(at, edgeDst, edgeData); }
  ptrdiff_t distance_to(const Self& other) const {
    return other.at - (ptrdiff_t)at;
  }
  void increment() { ++at; }
  void decrement() { --at; }
  void advance(ptrdiff_t n) { at += n; }
};

template <typename IDTy>
class IntrusiveID {
  IDTy id;

public:
  IDTy& getID() { return id; }
  void setID(size_t n) { id = n; }
};

template <>
class IntrusiveID<void> {
public:
  char getID() { return 0; }
  void setID(size_t) {}
};

//! Empty class for HasLockable optimization
class NoLockable {};

//! Separate types from definitions to allow incomplete types as NodeTy
template <typename NodeTy, bool HasLockable>
struct NodeInfoBaseTypes {
  typedef NodeTy& reference;
  typedef const NodeTy& const_reference;
};

template <bool HasLockable>
struct NodeInfoBaseTypes<void, HasLockable> {
  typedef void* reference;
  typedef void* const_reference;
};

//! Specializations for void node data
template <typename NodeTy, bool HasLockable>
class NodeInfoBase
    : public boost::mpl::if_c<HasLockable, katana::Lockable, NoLockable>::type,
      public NodeInfoBaseTypes<NodeTy, HasLockable> {
  NodeTy data;

public:
  template <typename... Args>
  NodeInfoBase(Args&&... args) : data(std::forward<Args>(args)...) {}

  typename NodeInfoBase::reference getData() { return data; }
  typename NodeInfoBase::const_reference getData() const { return data; }
};

template <bool HasLockable>
struct NodeInfoBase<void, HasLockable>
    : public boost::mpl::if_c<HasLockable, katana::Lockable, NoLockable>::type,
      public NodeInfoBaseTypes<void, HasLockable> {
  typename NodeInfoBase::reference getData() { return 0; }
  typename NodeInfoBase::const_reference getData() const { return 0; }
};

template <bool Enable>
class OutOfLineLockableFeature {
  typedef NodeInfoBase<void, true> OutOfLineLock;
  NUMAArray<OutOfLineLock> outOfLineLocks;

public:
  struct size_of_out_of_line {
    static const size_t value = sizeof(OutOfLineLock);
  };

  void outOfLineAcquire(size_t n, MethodFlag mflag) {
    katana::acquire(&outOfLineLocks[n], mflag);
  }
  void outOfLineAllocateLocal(size_t numNodes) {
    outOfLineLocks.allocateLocal(numNodes);
  }
  void outOfLineAllocateInterleaved(size_t numNodes) {
    outOfLineLocks.allocateInterleaved(numNodes);
  }
  void outOfLineAllocateBlocked(size_t numNodes) {
    outOfLineLocks.allocateBlocked(numNodes);
  }
  void outOfLineAllocateFloating(size_t numNodes) {
    outOfLineLocks.allocateFloating(numNodes);
  }

  template <typename RangeArrayType>
  void outOfLineAllocateSpecified(size_t n, RangeArrayType threadRanges) {
    outOfLineLocks.allocateSpecified(n, threadRanges);
  }

  void outOfLineConstructAt(size_t n) { outOfLineLocks.constructAt(n); }
};

template <>
class OutOfLineLockableFeature<false> {
public:
  struct size_of_out_of_line {
    static const size_t value = 0;
  };
  void outOfLineAcquire(size_t, MethodFlag) {}
  void outOfLineAllocateLocal(size_t) {}
  void outOfLineAllocateInterleaved(size_t) {}
  void outOfLineAllocateBlocked(size_t) {}
  void outOfLineAllocateFloating(size_t) {}
  void outOfLineConstructAt(size_t) {}
  template <typename RangeArrayType>
  void outOfLineAllocateSpecified(size_t, RangeArrayType) {}
};

//! Edge specialization for void edge data
template <typename NodeInfoPtrTy, typename EdgeTy>
struct EdgeInfoBase : public LazyObject<EdgeTy> {
  NodeInfoPtrTy dst;
};

template <typename ItTy>
StandardRange<NoDerefIterator<ItTy>>
make_no_deref_range(ItTy ii, ItTy ee) {
  return MakeStandardRange(
      make_no_deref_iterator(ii), make_no_deref_iterator(ee));
}

template <typename A, typename B, typename C, typename D, typename E>
void
swap(EdgeSortReference<A, B, C, D, E> a, EdgeSortReference<A, B, C, D, E> b) {
  auto aa = *a;
  auto bb = *b;
  a = bb;
  b = aa;
}

}  // namespace katana::internal

#endif
