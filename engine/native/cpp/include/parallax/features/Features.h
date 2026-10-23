// Keypoint detection, description and matching.
//
// The engine ships its own ORB-style pipeline (FAST-9 corners, Harris scoring,
// intensity-centroid orientation, steered BRIEF-256) rather than depending on
// OpenCV. That keeps the reconstruction core free of a ~200 MB native
// dependency, lets the whole pipeline run inside a host gtest binary, and
// avoids the patent/licence questions around SIFT variants. When the optional
// PARALLAX_WITH_OPENCV build flag is enabled the SfM front-end can additionally
// use OpenCV's SIFT for very wide baselines; the built-in path is always
// compiled and is what the regression tests exercise.
#pragma once

#include "parallax/core/Types.h"

#include <array>

namespace parallax {

/// 256-bit binary descriptor, stored as 4 x 64-bit words.
struct Descriptor {
  std::array<uint64_t, 4> bits{0, 0, 0, 0};

  /// Hamming distance in [0, 256].
  int distance(const Descriptor& other) const {
    int d = 0;
    for (int i = 0; i < 4; ++i) {
      d += __builtin_popcountll(bits[i] ^ other.bits[i]);
    }
    return d;
  }
};

struct Keypoint {
  Vec2 pt = Vec2::Zero();   ///< position in level-0 (full resolution) pixels
  float score = 0.f;        ///< Harris corner response
  float angle = 0.f;        ///< orientation in radians from the intensity centroid
  int octave = 0;           ///< pyramid level the corner was found at
};

struct FeatureSet {
  std::vector<Keypoint> keypoints;
  std::vector<Descriptor> descriptors;

  size_t size() const { return keypoints.size(); }
  bool empty() const { return keypoints.empty(); }
};

struct FeatureParams {
  int max_features = 2000;
  int fast_threshold = 20;      ///< intensity difference for the FAST test
  int min_fast_threshold = 7;   ///< relaxed threshold for low-contrast tiles
  int octaves = 4;
  double scale_factor = 1.2;
  int border = 24;              ///< keep descriptors fully inside the image
  /// Corners are spread over a grid so a single textured region cannot soak up
  /// the whole budget; this is what keeps bundle adjustment well conditioned.
  int grid_cols = 8;
  int grid_rows = 8;
};

/// A Gaussian image pyramid with `octaves` levels.
class ImagePyramid {
 public:
  ImagePyramid(const GrayImage& base, int octaves, double scale_factor);

  int levels() const { return static_cast<int>(levels_.size()); }
  const GrayBuffer& level(int i) const { return levels_[i]; }
  /// Factor to multiply level-i coordinates by to reach level 0.
  double scaleToBase(int i) const { return scales_[i]; }

 private:
  std::vector<GrayBuffer> levels_;
  std::vector<double> scales_;
};

/// Separable 5-tap Gaussian blur (sigma ~ 1.0).
GrayBuffer gaussianBlur5(const GrayImage& src);

/// Half-resolution downsample with a 2x2 box filter after blurring.
GrayBuffer downsampleHalf(const GrayImage& src);

/// Bilinear resize to an explicit size.
GrayBuffer resizeBilinear(const GrayImage& src, int out_w, int out_h);

/// FAST-9 corner test at a single pixel. Returns the number of contiguous
/// pixels on the Bresenham-16 circle that are all brighter or all darker.
int fastScore(const GrayImage& img, int x, int y, int threshold);

/// Harris corner response using a 3x3 Sobel gradient over a 7x7 window.
double harrisResponse(const GrayImage& img, int x, int y);

/// Intensity-centroid orientation over a circular patch of radius 15.
double intensityCentroidAngle(const GrayImage& img, int x, int y, int radius = 15);

/// Detect corners on a single image level (no pyramid, no descriptors).
std::vector<Keypoint> detectFast(const GrayImage& img, const FeatureParams& params);

/// Full detect + describe over a pyramid.
FeatureSet detectAndDescribe(const GrayImage& img, const FeatureParams& params = {});

/// Compute a steered BRIEF-256 descriptor for one keypoint on one level.
Descriptor computeBrief(const GrayImage& img, double x, double y, double angle);

struct Match {
  int query = -1;
  int train = -1;
  int distance = 256;
};

struct MatchParams {
  int max_distance = 64;      ///< reject matches above this Hamming distance
  double ratio = 0.8;         ///< Lowe's ratio test on the two best candidates
  bool cross_check = true;    ///< keep only mutual best matches
};

/// Brute-force Hamming matching with ratio test and optional cross-check.
std::vector<Match> matchFeatures(const FeatureSet& a, const FeatureSet& b,
                                 const MatchParams& params = {});

// ----------------------------------------------------------- frame scoring ---

struct FrameQuality {
  double sharpness = 0.0;      ///< variance of the Laplacian, higher is better
  double exposure = 0.0;       ///< mean intensity, 0..255
  double clipped_low = 0.0;    ///< fraction of pixels at/near 0
  double clipped_high = 0.0;   ///< fraction of pixels at/near 255
  double contrast = 0.0;       ///< intensity standard deviation
  bool acceptable = false;
};

struct FrameQualityThresholds {
  double min_sharpness = 90.0;
  double min_exposure = 35.0;
  double max_exposure = 225.0;
  double max_clipped = 0.22;
  double min_contrast = 16.0;
};

/// Score a frame for blur and exposure. Cheap enough to run per preview frame.
FrameQuality scoreFrame(const GrayImage& img, const FrameQualityThresholds& t = {});

/// Variance of the Laplacian over the image (subsampled for speed).
double laplacianVariance(const GrayImage& img, int step = 2);

/// Perceptual hash (64-bit) used to drop near-duplicate frames.
uint64_t differenceHash(const GrayImage& img);

inline int hammingDistance64(uint64_t a, uint64_t b) {
  return __builtin_popcountll(a ^ b);
}

}  // namespace parallax
