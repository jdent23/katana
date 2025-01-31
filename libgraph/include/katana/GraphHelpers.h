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
#ifndef KATANA_LIBGRAPH_KATANA_GRAPHHELPERS_H_
#define KATANA_LIBGRAPH_KATANA_GRAPHHELPERS_H_

#include <cassert>
#include <vector>

#include <boost/iterator/counting_iterator.hpp>

#include "katana/PropertyGraph.h"
#include "katana/config.h"
#include "katana/gIO.h"

namespace katana {

namespace internal {

template <typename GraphTy>
inline GraphTopology::edge_iterator
edge_begin(GraphTy& graph, uint32_t N) {
  return graph.topology().edges(N).begin();
}

/**
 * Gets the end edge boundary of some node.
 *
 * @param N node to get the edge of
 * @returns iterator to the end of the edges of node N, i.e. the first edge
 * of the next node (or an "end" iterator if there is no next node)
 */
template <typename GraphTy>
inline GraphTopology::edge_iterator
edge_end(GraphTy& graph, uint32_t N) {
  return graph.topology().edges(N).end();
}

template <typename Ty>
inline size_t
getEdgePrefixSum(Ty& p, size_t n) {
  return p[n];
}
template <>
inline size_t
getEdgePrefixSum<PropertyGraph>(PropertyGraph& p, size_t n) {
  return *edge_end(p, n);
}

/**
 * Return a suitable index between an upper bound and a lower bound that
 * attempts to get close to the target size (i.e. find a good chunk that
 * corresponds to some size) using a prefix sum.
 *
 * @tparam PrefixSumType type of the object that holds the edge prefix sum
 *
 * @param nodeWeight weight to give to a node in division
 * @param edgeWeight weight to give to an edge in division
 * @param targetWeight The amount of weight we want from the returned index
 * @param lb lower bound to start search from
 * @param ub upper bound to start search from
 * @param edgePrefixSum prefix sum of edges; may be full or partial prefix
 * sum of the object you are attempting to split
 * @param edgeOffset number of edges to subtract from edge count retrieved
 * from prefix sum; used if array is a partial prefix sum
 * @param nodeOffset number of nodes to skip over when looking in the
 * prefix sum: useful if the prefix sum is over the entire graph while you
 * just want to divide the nodes for a particular region (jump to the region
 * with the nodeOffset)
 *
 * @returns The node id that hits (or gets close to) the target size
 */
// Note: "inline" may be required if PrefixSumType is exactly the same type
// in 2 different translation units; otherwise it should be fine
template <typename PrefixSumType>
size_t
findIndexPrefixSum(
    size_t nodeWeight, size_t edgeWeight, size_t targetWeight, uint64_t lb,
    uint64_t ub, PrefixSumType& edgePrefixSum, uint64_t edgeOffset,
    uint64_t nodeOffset) {
  KATANA_LOG_DEBUG_ASSERT(nodeWeight != 0 || edgeWeight != 0);

  while (lb < ub) {
    size_t mid = lb + (ub - lb) / 2;
    size_t num_edges;

    if ((mid + nodeOffset) != 0) {
      num_edges =
          internal::getEdgePrefixSum(edgePrefixSum, mid - 1 + nodeOffset) -
          edgeOffset;
    } else {
      num_edges = 0;
    }

    size_t weight = num_edges * edgeWeight + mid * nodeWeight;

    if (weight < targetWeight) {
      lb = mid + 1;
    } else if (weight >= targetWeight) {
      ub = mid;
    }
  }

  return lb;
}

/**
 * Given a number of divisions and a scale factor specifying how much of a
 * chunk of blocks each division should get, determine the total number
 * of blocks to split among all divisions + calculate the prefix sum and
 * save it in-place to the scaleFactor variable.
 *
 * @param numDivisions number of divisions to split blocks among
 * @param scaleFactor vector specifying how much a particular vision should get
 *
 * @returns The total number of blocks to split among all divisions
 */
KATANA_EXPORT size_t
determine_block_division(size_t numDivisions, std::vector<size_t>& scaleFactor);

}  // end namespace internal

/**
 * Returns 2 ranges (one for nodes, one for edges) for a particular division.
 * The ranges specify the nodes/edges that a division is responsible for. The
 * function attempts to split them evenly among units given some kind of
 * weighting for both nodes and edges.
 *
 * Assumes the parameters passed in apply to a local portion of whatever
 * is being divided (i.e. concept of a "global" object is abstracted away in
 * some sense)
 *
 * @tparam PrefixSumType type of the object that holds the edge prefix sum
 * @tparam NodeType size of the type representing the node
 *
 * @param numNodes Total number of nodes included in prefix sum
 * @param numEdges Total number of edges included in prefix sum
 * @param nodeWeight weight to give to a node in division
 * @param edgeWeight weight to give to an edge in division
 * @param id Division number you want the range for
 * @param total Total number of divisions to divide nodes among
 * @param edgePrefixSum Prefix sum of the edges in the graph
 * @param scaleFactor Vector specifying if certain divisions should get more
 * than other divisions
 * @param edgeOffset number of edges to subtract from numbers in edgePrefixSum
 * @param nodeOffset number of nodes to skip over when looking in the
 * prefix sum: useful if the prefix sum is over the entire graph while you
 * just want to divide the nodes for a particular region (jump to the region
 * with the nodeOffset)
 *
 * @returns A node pair and an edge pair specifying the assigned nodes/edges
 * to division "id"; returns LOCAL ids, not global ids (i.e. if node offset
 * was used, it is up to the caller to add the offset to the numbers)
 */
template <typename PrefixSumType, typename NodeType = uint64_t>
auto
DivideNodesBinarySearch(
    NodeType numNodes, uint64_t numEdges, size_t nodeWeight, size_t edgeWeight,
    size_t id, size_t total, PrefixSumType& edgePrefixSum,
    std::vector<size_t> scaleFactor = std::vector<size_t>(),
    uint64_t edgeOffset = 0, uint64_t nodeOffset = 0) {
  typedef boost::counting_iterator<NodeType> iterator;
  typedef boost::counting_iterator<uint64_t> edge_iterator;
  typedef std::pair<iterator, iterator> NodeRange;
  typedef std::pair<edge_iterator, edge_iterator> EdgeRange;
  typedef std::pair<NodeRange, EdgeRange> GraphRange;

  // numNodes = 0 corner case
  if (numNodes == 0) {
    return GraphRange(
        NodeRange(iterator(0), iterator(0)),
        EdgeRange(edge_iterator(0), edge_iterator(0)));
  }

  KATANA_LOG_DEBUG_ASSERT(nodeWeight != 0 || edgeWeight != 0);
  KATANA_LOG_DEBUG_ASSERT(total >= 1);
  KATANA_LOG_DEBUG_ASSERT(id < total);

  // weight of all data
  uint64_t weight = numNodes * nodeWeight + (numEdges + 1) * edgeWeight;
  // determine the number of blocks to divide among total divisions + setup the
  // scale factor vector if necessary
  size_t numBlocks = internal::determine_block_division(total, scaleFactor);
  // weight of a block (one block for each division by default; if scale
  // factor specifies something different, then use that instead)
  uint64_t blockWeight = (weight + numBlocks - 1) / numBlocks;

  // lower and upper blocks that this division should use determined
  // using scaleFactor
  size_t blockLower;
  if (id != 0) {
    blockLower = scaleFactor[id - 1];
  } else {
    blockLower = 0;
  }

  size_t blockUpper = scaleFactor[id];

  KATANA_LOG_DEBUG_ASSERT(blockLower <= blockUpper);

  uint64_t nodesLower;
  // use prefix sum to find node bounds
  if (blockLower == 0) {
    nodesLower = 0;
  } else {
    nodesLower = internal::findIndexPrefixSum(
        nodeWeight, edgeWeight, blockWeight * blockLower, 0, numNodes,
        edgePrefixSum, edgeOffset, nodeOffset);
  }

  uint64_t nodesUpper;
  nodesUpper = internal::findIndexPrefixSum(
      nodeWeight, edgeWeight, blockWeight * blockUpper, nodesLower, numNodes,
      edgePrefixSum, edgeOffset, nodeOffset);

  // get the edges bounds using node lower/upper bounds
  uint64_t edgesLower = (nodesLower + nodeOffset == 0)
                            ? 0
                            : internal::getEdgePrefixSum(
                                  edgePrefixSum, nodesLower - 1 + nodeOffset) -
                                  edgeOffset;
  uint64_t edgesUpper = (nodesUpper + nodeOffset == 0)
                            ? 0
                            : internal::getEdgePrefixSum(
                                  edgePrefixSum, nodesUpper - 1 + nodeOffset) -
                                  edgeOffset;

  return GraphRange(
      NodeRange(iterator(nodesLower), iterator(nodesUpper)),
      EdgeRange(edge_iterator(edgesLower), edge_iterator(edgesUpper)));
}

// temporary overload for backwards-compatibility until all callers have been
// moved to the size_t version
template <typename PrefixSumType, typename NodeType = uint64_t>
auto
divideNodesBinarySearch(
    NodeType numNodes, uint64_t numEdges, size_t nodeWeight, size_t edgeWeight,
    size_t id, size_t total, PrefixSumType& edgePrefixSum,
    std::vector<unsigned> scaleFactor) {
  std::vector<size_t> sizeScaleFactor(scaleFactor.begin(), scaleFactor.end());
  return DivideNodesBinarySearch(
      numNodes, numEdges, nodeWeight, edgeWeight, id, total, edgePrefixSum,
      sizeScaleFactor);
}

// second internal namespace
namespace internal {

/**
 * Checks the begin/end node and number of units to split to for corner cases
 * (e.g. only one unit to split to, only 1 node, etc.).
 *
 * @param unitsToSplit number of units to split nodes among
 * @param beginNode Beginning of range
 * @param endNode End of range, non-inclusive
 * @param returnRanges vector to store result in
 * @returns true if a corner case was found (indicates that returnRanges has
 * been finalized)
 */
KATANA_EXPORT bool unitRangeCornerCaseHandle(
    uint32_t unitsToSplit, uint32_t beginNode, uint32_t endNode,
    std::vector<uint32_t>& returnRanges);

/**
 * Helper function used by determineUnitRangesGraph that consists of the main
 * loop over all units and calls to divide by node to determine the
 * division of nodes to units.
 *
 * Saves the ranges to an argument vector provided by the caller.
 *
 * @tparam GraphTy type of the graph object
 *
 * @param graph The graph object to get prefix sum information from
 * @param unitsToSplit number of units to split nodes among
 * @param beginNode Beginning of range
 * @param endNode End of range, non-inclusive
 * @param returnRanges Vector to store unit offsets for ranges in
 * @param nodeAlpha The higher the number, the more weight nodes have in
 * determining division of nodes (edges have weight 1).
 */
template <typename GraphTy>
void
determineUnitRangesLoopGraph(
    GraphTy& graph, uint32_t unitsToSplit, uint32_t beginNode, uint32_t endNode,
    std::vector<uint32_t>& returnRanges, uint32_t nodeAlpha) {
  KATANA_LOG_DEBUG_ASSERT(beginNode != endNode);

  uint32_t numNodesInRange = endNode - beginNode;
  // uint64_t numEdgesInRange =
  // graph.edge_end(endNode - 1) - graph.edge_begin(beginNode);
  uint64_t numEdgesInRange =
      edge_end(graph, endNode - 1) - edge_begin(graph, beginNode);
  uint64_t edgeOffset = *edge_begin(graph, beginNode);

  returnRanges[0] = beginNode;
  std::vector<size_t> dummyScaleFactor;

  for (uint32_t i = 0; i < unitsToSplit; i++) {
    // determine division for unit i
    auto nodeSplits =
        DivideNodesBinarySearch<GraphTy, uint32_t>(
            numNodesInRange, numEdgesInRange, nodeAlpha, 1, i, unitsToSplit,
            graph, dummyScaleFactor, edgeOffset, beginNode)
            .first;

    // i.e. if there are actually assigned nodes
    if (nodeSplits.first != nodeSplits.second) {
      if (i != 0) {
        KATANA_LOG_DEBUG_ASSERT(
            returnRanges[i] == *(nodeSplits.first) + beginNode);
      } else {  // i == 0
        KATANA_LOG_DEBUG_ASSERT(returnRanges[i] == beginNode);
      }
      returnRanges[i + 1] = *(nodeSplits.second) + beginNode;
    } else {
      // unit assinged no nodes, copy last one
      returnRanges[i + 1] = returnRanges[i];
    }
  }
}

/**
 * Helper function used by determineUnitRangesPrefixSum that consists of the
 * main loop over all units and calls to divide by node to determine the
 * division of nodes to units.
 *
 * Saves the ranges to an argument vector provided by the caller.
 *
 * @tparam VectorTy type of the prefix sum object
 *
 * @param prefixSum Holds prefix sum information
 * @param unitsToSplit number of units to split nodes among
 * @param beginNode Beginning of range
 * @param endNode End of range, non-inclusive
 * @param returnRanges Vector to store unit offsets for ranges in
 * @param nodeAlpha The higher the number, the more weight nodes have in
 * determining division of nodes (edges have weight 1).
 */
template <typename VectorTy>
void
determineUnitRangesLoopPrefixSum(
    VectorTy& prefixSum, uint32_t unitsToSplit, uint32_t beginNode,
    uint32_t endNode, std::vector<uint32_t>& returnRanges, uint32_t nodeAlpha) {
  KATANA_LOG_DEBUG_ASSERT(beginNode != endNode);

  uint32_t numNodesInRange = endNode - beginNode;

  uint64_t numEdgesInRange;
  uint64_t edgeOffset;
  if (beginNode != 0) {
    numEdgesInRange = prefixSum[endNode - 1] - prefixSum[beginNode - 1];
    edgeOffset = prefixSum[beginNode - 1];
  } else {
    numEdgesInRange = prefixSum[endNode - 1];
    edgeOffset = 0;
  }

  returnRanges[0] = beginNode;
  std::vector<size_t> dummyScaleFactor;

  for (uint32_t i = 0; i < unitsToSplit; i++) {
    // determine division for unit i
    auto nodeSplits =
        DivideNodesBinarySearch<VectorTy, uint32_t>(
            numNodesInRange, numEdgesInRange, nodeAlpha, 1, i, unitsToSplit,
            prefixSum, dummyScaleFactor, edgeOffset, beginNode)
            .first;

    // i.e. if there are actually assigned nodes
    if (nodeSplits.first != nodeSplits.second) {
      if (i != 0) {
        KATANA_LOG_DEBUG_ASSERT(
            returnRanges[i] == *(nodeSplits.first) + beginNode);
      } else {  // i == 0
        KATANA_LOG_DEBUG_ASSERT(returnRanges[i] == beginNode);
      }
      returnRanges[i + 1] = *(nodeSplits.second) + beginNode;
    } else {
      // unit assinged no nodes
      returnRanges[i + 1] = returnRanges[i];
    }
  }
}

/**
 * Sanity checks a finalized unit range vector.
 *
 * @param unitsToSplit number of units to split nodes among
 * @param beginNode Beginning of range
 * @param endNode End of range, non-inclusive
 * @param returnRanges Ranges to sanity check
 */
KATANA_EXPORT void unitRangeSanity(
    uint32_t unitsToSplit, uint32_t beginNode, uint32_t endNode,
    std::vector<uint32_t>& returnRanges);

}  // namespace internal

/**
 * Determines node division ranges for all nodes in a graph and returns it in
 * an offset vector. (node ranges = assigned nodes that a particular unit
 * of execution should work on)
 *
 * Checks for corner cases, then calls the main loop function.
 *
 * ONLY CALL AFTER GRAPH IS CONSTRUCTED as it uses functions that assume
 * the graph is already constructed.
 *
 * @tparam GraphTy type of the graph object
 *
 * @param graph The graph object to get prefix sum information from
 * @param unitsToSplit number of units to split nodes among
 * @param nodeAlpha The higher the number, the more weight nodes have in
 * determining division of nodes (edges have weight 1).
 * @returns vector that indirectly specifies which units get which nodes
 */
template <typename GraphTy>
std::vector<uint32_t>
determineUnitRangesFromGraph(
    GraphTy& graph, uint32_t unitsToSplit, uint32_t nodeAlpha = 0) {
  // uint32_t totalNodes = graph.size();
  uint32_t totalNodes = graph.topology().num_nodes();

  std::vector<uint32_t> returnRanges;
  returnRanges.resize(unitsToSplit + 1);

  // check corner cases
  if (internal::unitRangeCornerCaseHandle(
          unitsToSplit, 0, totalNodes, returnRanges)) {
    return returnRanges;
  }

  // no corner cases: onto main loop over nodes that determines
  // node ranges
  internal::determineUnitRangesLoopGraph(
      graph, unitsToSplit, 0, totalNodes, returnRanges, nodeAlpha);

  internal::unitRangeSanity(unitsToSplit, 0, totalNodes, returnRanges);

  return returnRanges;
}

/**
 * Determines node division ranges for a given range of nodes and returns it
 * as an offset vector. (node ranges = assigned nodes that a particular unit
 * of execution should work on)
 *
 * Checks for corner cases, then calls the main loop function.
 *
 * ONLY CALL AFTER GRAPH IS CONSTRUCTED as it uses functions that assume
 * the graph is already constructed.
 *
 * @tparam GraphTy type of the graph object
 *
 * @param graph The graph object to get prefix sum information from
 * @param unitsToSplit number of units to split nodes among
 * @param beginNode Beginning of range
 * @param endNode End of range, non-inclusive
 * @param nodeAlpha The higher the number, the more weight nodes have in
 * determining division of nodes (edges have weight 1).
 * @returns vector that indirectly specifies which units get which nodes
 */
template <typename GraphTy>
std::vector<uint32_t>
determineUnitRangesFromGraph(
    GraphTy& graph, uint32_t unitsToSplit, uint32_t beginNode, uint32_t endNode,
    uint32_t nodeAlpha = 0) {
  std::vector<uint32_t> returnRanges;
  returnRanges.resize(unitsToSplit + 1);

  if (internal::unitRangeCornerCaseHandle(
          unitsToSplit, beginNode, endNode, returnRanges)) {
    return returnRanges;
  }

  // no corner cases: onto main loop over nodes that determines
  // node ranges
  internal::determineUnitRangesLoopGraph(
      graph, unitsToSplit, beginNode, endNode, returnRanges, nodeAlpha);

  internal::unitRangeSanity(unitsToSplit, beginNode, endNode, returnRanges);

  return returnRanges;
}

/**
 * Uses the divideByNode function (which is binary search based) to
 * divide nodes among units using a provided prefix sum.
 *
 * @tparam VectorTy type of the prefix sum object
 *
 * @param unitsToSplit number of units to split nodes among
 * @param edgePrefixSum A prefix sum of edges
 * @param numNodes number of nodes in the graph
 * @param nodeAlpha amount of weight to give to nodes when dividing work among
 * threads
 * @returns vector that indirectly specifies how nodes are split amongs units
 * of execution
 */
template <typename VectorTy>
std::vector<uint32_t>
determineUnitRangesFromPrefixSum(
    uint32_t unitsToSplit, VectorTy& edgePrefixSum, uint64_t numNodes,
    uint32_t nodeAlpha = 0) {
  KATANA_LOG_DEBUG_ASSERT(unitsToSplit > 0);

  std::vector<uint32_t> nodeRanges;
  nodeRanges.resize(unitsToSplit + 1);

  nodeRanges[0] = 0;

  // handle corner case TODO there are better ways to do this, i.e. call helper
  if (numNodes == 0) {
    nodeRanges[0] = 0;

    for (uint32_t i = 0; i < unitsToSplit; i++) {
      nodeRanges[i + 1] = 0;
    }
    return nodeRanges;
  }

  uint64_t numEdges = internal::getEdgePrefixSum(
      edgePrefixSum, numNodes - 1);  // edgePrefixSum[numNodes - 1];

  for (uint32_t i = 0; i < unitsToSplit; i++) {
    auto nodeSplits =
        DivideNodesBinarySearch<VectorTy, uint32_t>(
            numNodes, numEdges, nodeAlpha, 1, i, unitsToSplit, edgePrefixSum)
            .first;

    // i.e. if there are actually assigned nodes
    if (nodeSplits.first != nodeSplits.second) {
      if (i != 0) {
        KATANA_LOG_DEBUG_ASSERT(nodeRanges[i] == *(nodeSplits.first));
      } else {  // i == 0
        KATANA_LOG_DEBUG_ASSERT(nodeRanges[i] == 0);
      }
      nodeRanges[i + 1] = *(nodeSplits.second);
    } else {
      // unit assinged no nodes
      nodeRanges[i + 1] = nodeRanges[i];
    }
  }

  return nodeRanges;
}

/**
 * Uses the divideByNode function (which is binary search based) to
 * divide nodes among units using a provided prefix sum. Provide a node range
 * so that the prefix sum is only calculated using that range.
 *
 * @tparam VectorTy type of the prefix sum object
 *
 * @param unitsToSplit number of units to split nodes among
 * @param edgePrefixSum A prefix sum of edges
 * @param beginNode Beginning of range
 * @param endNode End of range, non-inclusive
 * @param nodeAlpha amount of weight to give to nodes when dividing work among
 * threads
 * @returns vector that indirectly specifies how nodes are split amongs units
 * of execution
 */
template <typename VectorTy>
std::vector<uint32_t>
determineUnitRangesFromPrefixSum(
    uint32_t unitsToSplit, VectorTy& edgePrefixSum, uint32_t beginNode,
    uint32_t endNode, uint32_t nodeAlpha = 0) {
  std::vector<uint32_t> returnRanges;
  returnRanges.resize(unitsToSplit + 1);

  if (internal::unitRangeCornerCaseHandle(
          unitsToSplit, beginNode, endNode, returnRanges)) {
    return returnRanges;
  }

  // no corner cases: onto main loop over nodes that determines
  // node ranges
  internal::determineUnitRangesLoopPrefixSum(
      edgePrefixSum, unitsToSplit, beginNode, endNode, returnRanges, nodeAlpha);

  internal::unitRangeSanity(unitsToSplit, beginNode, endNode, returnRanges);

  return returnRanges;
}

}  // end namespace katana

#endif
