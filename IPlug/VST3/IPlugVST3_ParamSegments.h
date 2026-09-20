/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for  more info.

 ==============================================================================
 */

#pragma once

/**
 * @file
 * @brief Sample-accurate splitting of a VST3 audio block at the host's parameter automation points.
 *
 * VST3 hands the processor one queue of (sampleOffset, value) points per automated parameter. Applying only the
 * last point of each queue at the start of the block (what IPlugVST3ProcessorBase does by default) turns smooth
 * automation into one step per block, which is audible with large buffers (iPlug2 issue #780).
 *
 * This header is dependency free on purpose (no VST3 SDK, no iPlug headers) so that the splitting logic can be
 * unit tested on its own. It never allocates: the caller owns the point storage and passes callables, which are
 * called directly (no std::function), so it is safe to use on the audio thread.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace iplug
{

/** One automation point taken from one of the host's per-parameter queues */
struct VST3ParamPoint
{
  int32_t offset;  //!< sample offset inside the block, as reported by the host
  uint32_t seq;    //!< arrival order across all queues; keeps equal offsets deterministic
  int32_t paramID; //!< VST3 parameter ID
  double value;    //!< normalized value
};

/** Orders points by sample offset, then by arrival order. Allocation free (std::sort works in place). */
inline void SortVST3ParamPoints(VST3ParamPoint* pPoints, size_t nPoints)
{
  std::sort(pPoints, pPoints + nPoints, [](const VST3ParamPoint& a, const VST3ParamPoint& b) {
    return a.offset != b.offset ? a.offset < b.offset : a.seq < b.seq;
  });
}

/**
 * Splits a block of \c numSamples at the (already sorted) automation points.
 *
 * Points are applied by \c apply(const VST3ParamPoint&) right before the segment they belong to is processed by
 * \c process(int32_t startSample, int32_t numFrames). Every point that starts within \c minSegment samples of a
 * segment boundary is applied together at that boundary, so a point is applied at most minSegment-1 samples early and
 * never late, and no segment (except possibly the last one) is shorter than \c minSegment. That bounds the per-block
 * overhead however dense the host's automation is.
 *
 * Points at or beyond the end of the block are applied after the last segment, so the parameter state stays
 * consistent for the next block. With no points the whole block is processed as a single segment.
 *
 * @param pPoints points sorted with SortVST3ParamPoints()
 * @param minSegment shortest allowed segment in samples (clamped to at least 1)
 */
template <class ApplyFn, class ProcessFn>
void ProcessWithVST3ParamSegments(const VST3ParamPoint* pPoints, size_t nPoints, int32_t numSamples, int32_t minSegment, ApplyFn&& apply, ProcessFn&& process)
{
  const int64_t minSpan = std::max<int32_t>(minSegment, 1);
  int32_t pos = 0;
  size_t i = 0;

  while (pos < numSamples)
  {
    const int64_t groupEnd = static_cast<int64_t>(pos) + minSpan;

    while (i < nPoints && pPoints[i].offset < groupEnd)
      apply(pPoints[i++]);

    // Every remaining point starts at or after groupEnd, so segEnd is always > pos
    const int32_t segEnd = i < nPoints ? static_cast<int32_t>(std::min<int64_t>(pPoints[i].offset, numSamples)) : numSamples;

    process(pos, segEnd - pos);
    pos = segEnd;
  }

  while (i < nPoints)
    apply(pPoints[i++]);
}

} // namespace iplug
