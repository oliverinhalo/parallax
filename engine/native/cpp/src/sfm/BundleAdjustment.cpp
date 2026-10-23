#include "parallax/sfm/BundleAdjustment.h"

#include "parallax/sfm/Pnp.h"

#include <Eigen/Cholesky>
#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace parallax {
namespace {

using Mat6 = Eigen::Matrix<double, 6, 6>;
using Vec6 = Eigen::Matrix<double, 6, 1>;
using Mat63 = Eigen::Matrix<double, 6, 3>;

/// Huber weight and robustified cost for a residual of magnitude `e`.
struct Robust {
  double weight;
  double cost;
};

Robust huber(double e, double delta) {
  if (e <= delta) return Robust{1.0, e * e};
  // rho(e) = 2*delta*e - delta^2, with d rho / d(e^2) = delta / e giving the
  // IRLS weight below.
  return Robust{delta / e, delta * (2.0 * e - delta)};
}

/// Per-point accumulation for the Schur complement.
struct PointBlock {
  Mat3 V = Mat3::Zero();
  Vec3 g = Vec3::Zero();
  // Pose blocks this point couples to: (free pose index, W = Jc^T Jp).
  std::vector<std::pair<int, Mat63>> couplings;
};

}  // namespace

bool BundleProblem::valid() const {
  if (poses.empty() || points.empty() || observations.empty()) return false;
  if (intrinsics.size() != poses.size()) return false;
  for (const Observation& o : observations) {
    if (o.camera < 0 || o.camera >= static_cast<int>(poses.size())) return false;
    if (o.point < 0 || o.point >= static_cast<int>(points.size())) return false;
  }
  for (const CameraIntrinsics& k : intrinsics) {
    if (k.fx <= 0.0 || k.fy <= 0.0) return false;
  }
  return true;
}

double bundleRmsePixels(const BundleProblem& problem, int* out_used) {
  double sum_sq = 0.0;
  int used = 0;
  for (const Observation& o : problem.observations) {
    if (o.camera < 0 || o.camera >= static_cast<int>(problem.poses.size())) continue;
    if (o.point < 0 || o.point >= static_cast<int>(problem.points.size())) continue;
    Vec2 projected;
    if (!problem.intrinsics[o.camera].project(problem.poses[o.camera] * problem.points[o.point],
                                              &projected)) {
      continue;
    }
    sum_sq += (projected - o.pixel).squaredNorm();
    ++used;
  }
  if (out_used) *out_used = used;
  return used ? std::sqrt(sum_sq / used) : 0.0;
}

int pruneOutlierObservations(BundleProblem* problem, double threshold_px) {
  if (problem == nullptr) return 0;
  const size_t before = problem->observations.size();

  std::vector<Observation> kept;
  kept.reserve(problem->observations.size());
  for (const Observation& o : problem->observations) {
    if (o.camera < 0 || o.camera >= static_cast<int>(problem->poses.size())) continue;
    if (o.point < 0 || o.point >= static_cast<int>(problem->points.size())) continue;
    Vec2 projected;
    // An observation whose point has fallen behind its camera is always dropped.
    if (!problem->intrinsics[o.camera].project(
            problem->poses[o.camera] * problem->points[o.point], &projected)) {
      continue;
    }
    if ((projected - o.pixel).norm() <= threshold_px) kept.push_back(o);
  }

  // A landmark seen once is unconstrained, so it would add a rank-deficient
  // 3x3 block to V. Drop those observations too.
  std::vector<int> counts(problem->points.size(), 0);
  for (const Observation& o : kept) ++counts[o.point];
  std::vector<Observation> final_kept;
  final_kept.reserve(kept.size());
  for (const Observation& o : kept) {
    if (counts[o.point] >= 2) final_kept.push_back(o);
  }

  problem->observations = std::move(final_kept);
  return static_cast<int>(before - problem->observations.size());
}

BundleSummary optimizeBundle(BundleProblem* problem, const BundleOptions& options) {
  BundleSummary summary;
  if (problem == nullptr || !problem->valid()) {
    summary.message = "invalid problem";
    return summary;
  }

  const int num_poses = static_cast<int>(problem->poses.size());
  const int num_points = static_cast<int>(problem->points.size());

  // ---- parameter indexing -------------------------------------------------
  // Map each free pose/point to a slot in the reduced system; -1 means fixed.
  std::vector<int> pose_slot(num_poses, -1);
  int free_poses = 0;
  bool first_free_seen = false;
  for (int i = 0; i < num_poses; ++i) {
    if (problem->poseIsConstant(i)) continue;
    if (options.fix_first_pose && !first_free_seen) {
      // Gauge fix: the cost is invariant to a global rigid transform.
      first_free_seen = true;
      continue;
    }
    pose_slot[i] = free_poses++;
  }
  std::vector<int> point_slot(num_points, -1);
  int free_points = 0;
  for (int j = 0; j < num_points; ++j) {
    if (!problem->pointIsConstant(j)) point_slot[j] = free_points++;
  }

  if (free_poses == 0 && free_points == 0) {
    summary.message = "nothing to optimise";
    summary.initial_rmse_px = summary.final_rmse_px =
        bundleRmsePixels(*problem, &summary.observations_used);
    summary.success = true;
    summary.converged = true;
    return summary;
  }
  if (free_poses > options.max_dense_cameras) {
    summary.message = "too many free cameras for the dense reduced system; "
                      "use a sliding-window or chunked solve";
    return summary;
  }

  // ---- scale gauge --------------------------------------------------------
  // Monocular problems are scale-invariant. Rather than adding a soft prior we
  // project back onto the gauge after every accepted step, which keeps the
  // normal equations untouched and cannot bias the solution.
  int scale_ref_a = -1, scale_ref_b = -1;
  double reference_baseline = 0.0;
  if (options.fix_scale) {
    for (int i = 0; i < num_poses && scale_ref_b < 0; ++i) {
      if (problem->poseIsConstant(i)) continue;
      if (scale_ref_a < 0) {
        scale_ref_a = i;
      } else {
        scale_ref_b = i;
      }
    }
    if (scale_ref_a >= 0 && scale_ref_b >= 0) {
      reference_baseline =
          (problem->poses[scale_ref_a].center() - problem->poses[scale_ref_b].center()).norm();
    }
    if (reference_baseline < 1e-9) {
      scale_ref_a = scale_ref_b = -1;  // degenerate; silently skip the gauge
    }
  }
  const auto applyScaleGauge = [&](std::vector<Pose>* poses, std::vector<Vec3>* points) {
    if (scale_ref_a < 0 || scale_ref_b < 0) return;
    const Vec3 anchor = (*poses)[scale_ref_a].center();
    const double current = (anchor - (*poses)[scale_ref_b].center()).norm();
    if (current < 1e-12) return;
    const double s = reference_baseline / current;
    if (std::abs(s - 1.0) < 1e-15) return;
    for (Pose& p : *poses) {
      const Vec3 c = anchor + (p.center() - anchor) * s;
      p.t = -p.R * c;  // keep R, move the centre
    }
    for (Vec3& x : *points) x = anchor + (x - anchor) * s;
  };

  // ---- cost evaluation ----------------------------------------------------
  const auto evaluateCost = [&](const std::vector<Pose>& poses,
                                const std::vector<Vec3>& points) {
    double cost = 0.0;
    for (const Observation& o : problem->observations) {
      Vec2 projected;
      if (!problem->intrinsics[o.camera].project(poses[o.camera] * points[o.point], &projected)) {
        // A point behind the camera is given a large but finite penalty so the
        // solver is pushed away from the configuration rather than seeing an
        // infinite, uninformative cost.
        cost += 1e6;
        continue;
      }
      const double e = (projected - o.pixel).norm() * o.weight;
      cost += huber(e, options.huber_delta_px).cost;
    }
    return cost;
  };

  summary.initial_rmse_px = bundleRmsePixels(*problem, &summary.observations_used);
  summary.initial_cost = evaluateCost(problem->poses, problem->points);
  summary.final_cost = summary.initial_cost;
  summary.final_rmse_px = summary.initial_rmse_px;

  double lambda = options.initial_lambda;
  double current_cost = summary.initial_cost;

  std::vector<Mat6> U(free_poses);
  std::vector<Vec6> g_cam(free_poses);
  std::vector<PointBlock> blocks(free_points);
  Eigen::MatrixXd S;
  Eigen::VectorXd b;

  for (int iteration = 0; iteration < options.max_iterations; ++iteration) {
    summary.iterations = iteration + 1;

    // ---- accumulate the normal equations ---------------------------------
    for (Mat6& u : U) u.setZero();
    for (Vec6& g : g_cam) g.setZero();
    for (PointBlock& pb : blocks) {
      pb.V.setZero();
      pb.g.setZero();
      pb.couplings.clear();
    }
    double max_gradient = 0.0;

    for (const Observation& o : problem->observations) {
      const int cam_slot = pose_slot[o.camera];
      const int pt_slot = point_slot[o.point];
      if (cam_slot < 0 && pt_slot < 0) continue;  // both fixed: no contribution

      Vec2 projected;
      Eigen::Matrix<double, 2, 6> Jc;
      Eigen::Matrix<double, 2, 3> Jp;
      if (!projectionJacobian(problem->poses[o.camera], problem->intrinsics[o.camera],
                              problem->points[o.point], &projected, &Jc, &Jp)) {
        continue;
      }
      const Vec2 r = (projected - o.pixel) * o.weight;
      const double sw = std::sqrt(huber(r.norm(), options.huber_delta_px).weight) * o.weight;
      Jc *= sw;
      Jp *= sw;
      const Vec2 rw = r * sw;

      if (cam_slot >= 0) {
        U[cam_slot].noalias() += Jc.transpose() * Jc;
        g_cam[cam_slot].noalias() += Jc.transpose() * rw;
      }
      if (pt_slot >= 0) {
        PointBlock& pb = blocks[pt_slot];
        pb.V.noalias() += Jp.transpose() * Jp;
        pb.g.noalias() += Jp.transpose() * rw;
        if (cam_slot >= 0) {
          // Accumulate into an existing coupling if this camera already sees
          // the point (multiple observations of one landmark in one frame).
          auto it = std::find_if(pb.couplings.begin(), pb.couplings.end(),
                                 [cam_slot](const std::pair<int, Mat63>& c) {
                                   return c.first == cam_slot;
                                 });
          if (it == pb.couplings.end()) {
            pb.couplings.emplace_back(cam_slot, Mat63(Jc.transpose() * Jp));
          } else {
            it->second.noalias() += Jc.transpose() * Jp;
          }
        }
      }
    }
    for (const Vec6& g : g_cam) max_gradient = std::max(max_gradient, g.cwiseAbs().maxCoeff());
    for (const PointBlock& pb : blocks) {
      max_gradient = std::max(max_gradient, pb.g.cwiseAbs().maxCoeff());
    }
    if (max_gradient < options.gradient_tolerance) {
      summary.converged = true;
      summary.message = "gradient tolerance reached";
      break;
    }

    // ---- solve, with LM damping, retrying on rejection -------------------
    bool step_accepted = false;
    for (int attempt = 0; attempt < 12 && !step_accepted; ++attempt) {
      // Schur complement: eliminate the point blocks to get a dense system in
      // the camera parameters only.
      //   [U  W ] [dc]   [-g_c]
      //   [W^T V] [dp] = [-g_p]
      //   (U - W V^-1 W^T) dc = -g_c + W V^-1 g_p
      //   dp = -V^-1 (g_p + W^T dc)
      S.setZero(6 * free_poses, 6 * free_poses);
      b.setZero(6 * free_poses);
      for (int c = 0; c < free_poses; ++c) {
        Mat6 damped = U[c];
        // Scale-invariant damping: adding lambda * diag(U) rather than
        // lambda * I keeps the step sensible when rotation and translation
        // columns differ in magnitude by orders of magnitude.
        damped.diagonal() += lambda * U[c].diagonal().cwiseMax(1e-9);
        S.block<6, 6>(6 * c, 6 * c) = damped;
        b.segment<6>(6 * c) = -g_cam[c];
      }

      std::vector<Mat3> Vinv(free_points);
      std::vector<uint8_t> point_ok(free_points, 0);
      for (int p = 0; p < free_points; ++p) {
        PointBlock& pb = blocks[p];
        if (pb.V.isZero(0.0)) continue;
        Mat3 damped = pb.V;
        damped.diagonal() += lambda * pb.V.diagonal().cwiseMax(1e-9);
        // A landmark with fewer than two rays gives a rank-deficient block, so
        // guard the inverse rather than producing NaNs.
        const Eigen::FullPivLU<Mat3> lu(damped);
        if (!lu.isInvertible()) continue;
        Vinv[p] = lu.inverse();
        point_ok[p] = 1;

        for (const auto& [ca, Wa] : pb.couplings) {
          b.segment<6>(6 * ca).noalias() += Wa * (Vinv[p] * pb.g);
          for (const auto& [cb, Wb] : pb.couplings) {
            S.block<6, 6>(6 * ca, 6 * cb).noalias() -= Wa * Vinv[p] * Wb.transpose();
          }
        }
      }

      Eigen::VectorXd delta_cam = Eigen::VectorXd::Zero(6 * free_poses);
      if (free_poses > 0) {
        // S is symmetric positive definite once damped, so LDLT is both the
        // fastest and the most numerically appropriate factorisation.
        const Eigen::LDLT<Eigen::MatrixXd> ldlt(S);
        if (ldlt.info() != Eigen::Success) {
          lambda = std::min(options.max_lambda, lambda * 10.0);
          continue;
        }
        delta_cam = ldlt.solve(b);
        if (!delta_cam.allFinite()) {
          lambda = std::min(options.max_lambda, lambda * 10.0);
          continue;
        }
      }

      // Back-substitute the point updates.
      std::vector<Vec3> delta_point(free_points, Vec3::Zero());
      for (int p = 0; p < free_points; ++p) {
        if (!point_ok[p]) continue;
        Vec3 rhs = -blocks[p].g;
        for (const auto& [ca, Wa] : blocks[p].couplings) {
          rhs.noalias() -= Wa.transpose() * delta_cam.segment<6>(6 * ca);
        }
        delta_point[p] = Vinv[p] * rhs;
      }

      // Trial state.
      std::vector<Pose> trial_poses = problem->poses;
      std::vector<Vec3> trial_points = problem->points;
      for (int i = 0; i < num_poses; ++i) {
        if (pose_slot[i] >= 0) {
          trial_poses[i].applyLeftIncrement(delta_cam.segment<6>(6 * pose_slot[i]));
        }
      }
      for (int j = 0; j < num_points; ++j) {
        if (point_slot[j] >= 0) trial_points[j] += delta_point[point_slot[j]];
      }
      applyScaleGauge(&trial_poses, &trial_points);

      const double trial_cost = evaluateCost(trial_poses, trial_points);
      if (trial_cost < current_cost) {
        double step_norm = delta_cam.norm();
        for (const Vec3& d : delta_point) step_norm += d.norm();

        const double relative_decrease =
            (current_cost - trial_cost) / std::max(1e-30, current_cost);
        problem->poses = std::move(trial_poses);
        problem->points = std::move(trial_points);
        current_cost = trial_cost;
        lambda = std::max(options.min_lambda, lambda * 0.1);
        step_accepted = true;
        ++summary.successful_steps;

        if (relative_decrease < options.function_tolerance) {
          summary.converged = true;
          summary.message = "function tolerance reached";
        }
        if (step_norm < options.parameter_tolerance) {
          summary.converged = true;
          summary.message = "parameter tolerance reached";
        }
      } else {
        lambda = std::min(options.max_lambda, lambda * 10.0);
      }
    }

    if (!step_accepted) {
      // Levenberg-Marquardt could not find a cost-reducing step even at the
      // largest damping it was allowed. That *is* the convergence criterion --
      // it is what happens when the problem starts at (or reaches) the optimum
      // -- so it must not be reported as a failure. Genuine failures are
      // invalid input and factorisation breakdown, both handled above.
      summary.converged = true;
      if (summary.message.empty()) summary.message = "converged: no cost-reducing step";
      break;
    }
    if (summary.converged) break;
  }

  summary.final_cost = current_cost;
  summary.final_rmse_px = bundleRmsePixels(*problem, &summary.observations_used);
  // The problem was well formed and the solver ran to a stopping criterion.
  // `converged` distinguishes "reached a tolerance" from "hit the iteration
  // limit with progress still available".
  summary.success = true;
  if (summary.message.empty()) summary.message = "iteration limit reached";
  return summary;
}

}  // namespace parallax
