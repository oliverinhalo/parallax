#pragma once

#include <vector>

#include <cstddef>

namespace parallax {

/// Nothing useful comes out of a band shallower than this, whatever the depth maps say.
constexpr float kMinIntegrationDepthM = 0.6f;

/**
 * How far to integrate depth, from the depth maps rather than from the capture mode.
 *
 * A fixed per-mode range was wrong in both directions. Object scans integrated to 1.6 m, so
 * someone standing back from a chair had every sample dropped and sixty-nine keyframes fused
 * into an empty volume; rooms integrated to six, which spends a finite voxel budget on the far
 * wall behind the sofa. Where the surface is is a property of the capture, and the depth maps
 * are a measurement of it.
 *
 * @param samples valid depths in metres, gathered from across the keyframes. Zero and negative
 *   values are the caller's to exclude.
 * @param ceiling the mode's limit, as a guard against a depth map full of nonsense rather than
 *   as the working range. Applied after the measurement, so it can only ever reduce it.
 *
 * The 85th percentile, with a quarter added: a percentile because a handful of samples always
 * run off to the horizon through a window or past the edge of a table and those must not set
 * the range, and the margin so the furthest real surface sits inside the band rather than on
 * its boundary.
 */
float integrationDepthFrom(std::vector<float>* samples, float ceiling);

}  // namespace parallax
