/*
 * Copyright (C) 2019 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#include "geometry/ShapeInternal.hpp"
#include "DetectConflictInternal.hpp"
#include "Spline.hpp"
#include "StaticMotion.hpp"

#include <rmf_traffic/Conflict.hpp>

#include <fcl/continuous_collision.h>
#include <fcl/ccd/motion.h>

#include <unordered_map>

namespace rmf_traffic {

//==============================================================================
class ConflictData::Implementation
{
public:

  Time time;
  Segments segments;

};

//==============================================================================
Time ConflictData::get_time() const
{
  return _pimpl->time;
}

//==============================================================================
const ConflictData::Segments& ConflictData::get_segments() const
{
  return _pimpl->segments;
}

//==============================================================================
ConflictData::ConflictData()
{
  // Do nothing
}

//==============================================================================
class invalid_trajectory_error::Implementation
{
public:

  std::string what;

  static invalid_trajectory_error make_segment_num_error(
      std::size_t num_segments)
  {
    invalid_trajectory_error error;
    error._pimpl->what = std::string()
        + "[rmf_traffic::invalid_trajectory_error] Attempted to check a "
        + "conflict with a Trajectory that has [" + std::to_string(num_segments)
        + "] segments. This is not supported. Trajectories must have at least "
        + "2 segments to check them for conflicts.";
    return error;
  }

  static invalid_trajectory_error make_missing_shape_error(
      const Time time)
  {
    invalid_trajectory_error error;
    error._pimpl->what = std::string()
        + "[rmf_traffic::invalid_trajectory_error] Attempting to check a "
        + "conflict with a Trajectory that has no shape specified for the "
        + "profile of its segment at time ["
        + std::to_string(time.time_since_epoch().count())
        + "ns]. This is not supported.";

    return error;
  }
};

//==============================================================================
const char* invalid_trajectory_error::what() const noexcept
{
  return _pimpl->what.c_str();
}

//==============================================================================
invalid_trajectory_error::invalid_trajectory_error()
  : _pimpl(rmf_utils::make_impl<Implementation>())
{
  // This constructor is a no-op, but we'll keep a definition for it in case we
  // need it in the future. Allowing the default constructor to be inferred
  // could cause issues if we want to change the implementation of this
  // exception in the future, like if we want to add more information to the
  // error message output.
}

//==============================================================================
std::vector<ConflictData> DetectConflict::between(
    const Trajectory& trajectory_a,
    const Trajectory& trajectory_b,
    const bool quit_after_one)
{
  if(!broad_phase(trajectory_a, trajectory_b))
    return {};

  return narrow_phase(trajectory_a, trajectory_b, quit_after_one);
}

//==============================================================================

namespace {

struct BoundingBox
{
  Eigen::Vector2d min;
  Eigen::Vector2d max;
};

double evaluate_spline(
    const Eigen::Vector4d& coeffs,
    const double t)
{
  // Assume time is parameterized [0,1]
  return (coeffs[3] * t * t * t
      + coeffs[2] * t * t
      + coeffs[1] * t
      + coeffs[0]);
}

std::array<double, 2> get_local_extrema(
    const Eigen::Vector4d& coeffs)
{
  std::vector<double> extrema_candidates;
  // Store boundary values as potential extrema
  extrema_candidates.emplace_back(evaluate_spline(coeffs, 0));
  extrema_candidates.emplace_back(evaluate_spline(coeffs, 1));

  // When derivate of spline motion is not quadratic
  if (std::abs(coeffs[3]) < 1e-12)
  {
    if (std::abs(coeffs[2]) > 1e-12)
    {
      double t = -coeffs[1] / (2 * coeffs[2]);
      extrema_candidates.emplace_back(evaluate_spline(coeffs, t));
    }
  }
  else
  {
    // Calculate the discriminant otherwise
    double D = (4 * pow(coeffs[2], 2) - 12 * coeffs[3] * coeffs[1]);


    if (std::abs(D) < 1e-12)
    {
      double t = (-2 * coeffs[2]) / (6 * coeffs[3]);
      double extrema = evaluate_spline(coeffs, t);
      extrema_candidates.emplace_back(extrema);
    }
    else if (D < 0)
    {
      assert(false);
    }
    else
    {
      double t1 = ((-2 * coeffs[2]) + std::sqrt(D)) / (6 * coeffs[3]);
      double t2 = ((-2 * coeffs[2]) - std::sqrt(D)) / (6 * coeffs[3]);

      extrema_candidates.emplace_back(evaluate_spline(coeffs, t1));
      extrema_candidates.emplace_back(evaluate_spline(coeffs, t2));
    }
  }
  
  std::array<double, 2> extrema;
  assert(!extrema_candidates.empty());
  extrema[0] = *std::min_element(
      extrema_candidates.begin(),
      extrema_candidates.end());
  extrema[1] = *std::max_element(
      extrema_candidates.begin(),
      extrema_candidates.end());

  return extrema;
}

BoundingBox get_bounding_box(const rmf_traffic::Spline& spline)
{
  BoundingBox bounding_box;

  auto params = spline.get_params();
  std::array<double, 2> extrema_x = get_local_extrema(params.coeffs[0]);
  std::array<double, 2> extrema_y =  get_local_extrema(params.coeffs[1]);

  Eigen::Vector2d min_coord = Eigen::Vector2d{extrema_x[0], extrema_y[0]};
  Eigen::Vector2d max_coord = Eigen::Vector2d{extrema_x[1], extrema_y[1]};

  double char_length =  params.profile_ptr->get_shape()
      ->get_characteristic_length();

  assert(char_length >= 0.0);
  min_coord -= Eigen::Vector2d{char_length, char_length};
  max_coord += Eigen::Vector2d{char_length, char_length};

  bounding_box.min = min_coord;
  bounding_box.max = max_coord;

  return bounding_box;
}

bool overlap(const BoundingBox& box_a, const BoundingBox& box_b)
{
  for (std::size_t i=0; i < 2; ++i)
  {
    if (box_a.max[i] < box_b.min[i])
      return false;

    if (box_b.max[i] < box_a.min[i])
      return false;
  }

  return true;
}

//==============================================================================
std::shared_ptr<fcl::SplineMotion> make_uninitialized_fcl_spline_motion()
{
  // This function is only necessary because SplineMotion does not provide a
  // default constructor, and we want to be able to instantiate one before
  // we have any paramters to provide to it.
  fcl::Matrix3f R;
  fcl::Vec3f T;

  // The constructor that we are using is a no-op (apparently it was declared,
  // but its definition is just `// TODO`, so we don't need to worry about
  // unintended consequences. If we update the version of FCL, this may change,
  // so I'm going to leave a FIXME tag here to keep us aware of that.
  return std::make_shared<fcl::SplineMotion>(R, T, R, T);
}

//==============================================================================
std::tuple<Trajectory::const_iterator, Trajectory::const_iterator>
get_initial_iterators(
    const Trajectory& trajectory_a,
    const Trajectory& trajectory_b)
{
  std::size_t min_size = std::min(trajectory_a.size(), trajectory_b.size());
  if(min_size < 2)
  {
    throw invalid_trajectory_error::Implementation
        ::make_segment_num_error(min_size);
  }

  const Time& t_a0 = *trajectory_a.start_time();
  const Time& t_b0 = *trajectory_b.start_time();

  Trajectory::const_iterator a_it;
  Trajectory::const_iterator b_it;

  if(t_a0 < t_b0)
  {
    // Trajectory `a` starts first, so we begin evaluating at the time
    // that `b` begins
    a_it = trajectory_a.find(t_b0);
    b_it = ++trajectory_b.begin();
  }
  else if(t_b0 < t_a0)
  {
    // Trajectory `b` starts first, so we begin evaluating at the time
    // that `a` begins
    a_it = ++trajectory_a.begin();
    b_it = trajectory_b.find(t_a0);
  }
  else
  {
    // The Trajectories begin at the exact same time, so both will begin
    // from their start
    a_it = ++trajectory_a.begin();
    b_it = ++trajectory_b.begin();
  }

  return {a_it, b_it};
}

//==============================================================================
fcl::ContinuousCollisionRequest make_fcl_request()
{
  fcl::ContinuousCollisionRequest request;
  request.ccd_solver_type = fcl::CCDC_CONSERVATIVE_ADVANCEMENT;
  request.gjk_solver_type = fcl::GST_LIBCCD;

  return request;
}

} // anonymous namespace

bool DetectConflict::broad_phase(
    const Trajectory& trajectory_a,
    const Trajectory& trajectory_b)
{
  std::size_t min_size = std::min(trajectory_a.size(), trajectory_b.size());
  if(min_size < 2)
  {
    throw invalid_trajectory_error::Implementation
        ::make_segment_num_error(min_size);
  }

  if(trajectory_a.get_map_name() != trajectory_b.get_map_name())
    return false;

  const auto* t_a0 = trajectory_a.start_time();
  const auto* t_bf = trajectory_b.finish_time();

  // Neither of these can be null, because both trajectories should have at
  // least two elements.
  assert(t_a0 != nullptr);
  assert(t_bf != nullptr);

  if(*t_bf < *t_a0)
  {
    // If Trajectory `b` finishes before Trajectory `a` starts, then there
    // cannot be any conflict.
    return false;
  }

  const auto* t_b0 = trajectory_b.start_time();
  const auto* t_af = trajectory_a.finish_time();

  // Neither of these can be null, because both trajectories should have at
  // least two elements.
  assert(t_b0 != nullptr);
  assert(t_af != nullptr);

  if(*t_af < *t_b0)
  {
    // If Trajectory `a` finished before Trajectory `b` starts, then there
    // cannot be any conflict.
    return false;
  }

  // Iterate through the segments of both trajectories to check for overlapping
  // bounding boxes
  Trajectory::const_iterator a_it;
  Trajectory::const_iterator b_it;
  std::tie(a_it, b_it) = get_initial_iterators(trajectory_a, trajectory_b);
  assert(a_it != trajectory_a.end());
  assert(b_it != trajectory_b.end());

  Spline spline_a(a_it);
  Spline spline_b(b_it);

  while(a_it != trajectory_a.end() && b_it != trajectory_b.end())
  {
    // Increment a_it until spline_a will overlap with spline_b
    if(a_it->get_finish_time() < spline_b.start_time())
    {
      ++a_it;
      continue;
    }

    // Increment b_it until spline_b will overlap with spline_a
    if(b_it->get_finish_time() < spline_a.start_time())
    {
      ++b_it;
      continue;
    }

    spline_a = Spline(a_it);
    spline_b = Spline(b_it);

    auto box_a = get_bounding_box(spline_a);
    auto box_b = get_bounding_box(spline_b);

    if (overlap(box_a, box_b))
      return true;

    if(spline_a.finish_time() < spline_b.finish_time())
    {
      ++a_it;
    }
    else if(spline_b.finish_time() < spline_a.finish_time())
    {
      ++b_it;
    }
    else
    {
      ++a_it;
      ++b_it;
    }
  }
  return false;
}

class DetectConflict::Implementation
{
public:
  static ConflictData make_conflict(Time time, ConflictData::Segments segments)
  {
    ConflictData result;
    result._pimpl = rmf_utils::make_impl<ConflictData::Implementation>(
          ConflictData::Implementation{time, std::move(segments)});

    return result;
  }
};

//==============================================================================
std::vector<ConflictData> DetectConflict::narrow_phase(
    const Trajectory& trajectory_a,
    const Trajectory& trajectory_b,
    const bool quit_after_one)
{
  Trajectory::const_iterator a_it;
  Trajectory::const_iterator b_it;
  std::tie(a_it, b_it) = get_initial_iterators(trajectory_a, trajectory_b);

  // Verify that neither trajectory has run into a bug. These conditions should
  // be guaranteed by
  // 1. The assumption that the trajectories overlap (this is an assumption that
  //    is made explicit to the user)
  // 2. The min_size check up above
  assert(a_it != trajectory_a.end());
  assert(b_it != trajectory_b.end());

  // Initialize the objects that will be used inside the loop
  Spline spline_a(a_it);
  Spline spline_b(b_it);
  std::shared_ptr<fcl::SplineMotion> motion_a =
      make_uninitialized_fcl_spline_motion();
  std::shared_ptr<fcl::SplineMotion> motion_b =
      make_uninitialized_fcl_spline_motion();

  const fcl::ContinuousCollisionRequest request = make_fcl_request();
  fcl::ContinuousCollisionResult result;
  std::vector<ConflictData> conflicts;

  while(a_it != trajectory_a.end() && b_it != trajectory_b.end())
  {
    // Increment a_it until spline_a will overlap with spline_b
    if(a_it->get_finish_time() < spline_b.start_time())
    {
      ++a_it;
      continue;
    }

    // Increment b_it until spline_b will overlap with spline_a
    if(b_it->get_finish_time() < spline_a.start_time())
    {
      ++b_it;
      continue;
    }

    const Trajectory::ConstProfilePtr profile_a = a_it->get_profile();
    const Trajectory::ConstProfilePtr profile_b = b_it->get_profile();

    // TODO(MXG): Consider using optional<Spline> so that we can easily keep
    // track of which needs to be updated. There's some wasted computational
    // cycles here whenever we are using the same spline as a previous iteration
    spline_a = Spline(a_it);
    spline_b = Spline(b_it);

    const Time start_time =
        std::max(spline_a.start_time(), spline_b.start_time());
    const Time finish_time =
        std::min(spline_a.finish_time(), spline_b.finish_time());

    *motion_a = spline_a.to_fcl(start_time, finish_time);
    *motion_b = spline_b.to_fcl(start_time, finish_time);

    assert(profile_a->get_shape());
    assert(profile_b->get_shape());
    const auto obj_a = fcl::ContinuousCollisionObject(
          geometry::FinalConvexShape::Implementation::get_collision(
            *profile_a->get_shape()), motion_a);
    const auto obj_b = fcl::ContinuousCollisionObject(
          geometry::FinalConvexShape::Implementation::get_collision(
            *profile_b->get_shape()), motion_b);

    fcl::collide(&obj_a, &obj_b, request, result);
    if(result.is_collide)
    {
      const double scaled_time = result.time_of_contact;
      const Duration delta_t{
        Duration::rep(scaled_time * (finish_time - start_time).count())};
      const Time time = start_time + delta_t;
      conflicts.emplace_back(Implementation::make_conflict(time, {a_it, b_it}));
      if (quit_after_one)
        return conflicts;
    }

    if(spline_a.finish_time() < spline_b.finish_time())
    {
      ++a_it;
    }
    else if(spline_b.finish_time() < spline_a.finish_time())
    {
      ++b_it;
    }
    else
    {
      ++a_it;
      ++b_it;
    }
  }

  return conflicts;
}

namespace internal {
//==============================================================================
bool detect_conflicts(
    const Trajectory& trajectory,
    const Spacetime& region,
    std::vector<Trajectory::const_iterator>* output_iterators)
{
#ifndef NDEBUG
  // This should never actually happen because this function only gets used
  // internally, and so there should be several layers of quality checks on the
  // trajectories to prevent this. But we'll put it in here just in case.
  if(trajectory.size() < 2)
  {
    std::cerr << "[rmf_traffic::internal::detect_conflicts] An invalid "
              << "trajectory was passed to detect_conflicts. This is a bug "
              << "that should never happen. Please alert the RMF developers."
              << std::endl;
    throw invalid_trajectory_error::Implementation
        ::make_segment_num_error(trajectory.size());
  }
#endif // NDEBUG

  const Time trajectory_start_time = *trajectory.start_time();
  const Time trajectory_finish_time = *trajectory.finish_time();

  const Time start_time = region.lower_time_bound?
        std::max(*region.lower_time_bound, trajectory_start_time)
      : trajectory_start_time;

  const Time finish_time = region.upper_time_bound?
        std::min(*region.upper_time_bound, trajectory_finish_time)
      : trajectory_finish_time;

  if(finish_time < start_time)
  {
    // If the trajectory or region finishes before the other has started, that
    // means there is no overlap in time between the region and the trajectory,
    // so it is impossible for them to conflict.
    return false;
  }

  const Trajectory::const_iterator begin_it =
      trajectory_start_time < start_time?
        trajectory.find(start_time) : ++trajectory.begin();

  const Trajectory::const_iterator end_it =
      finish_time < trajectory_finish_time?
        ++trajectory.find(finish_time) : trajectory.end();

  std::shared_ptr<fcl::SplineMotion> motion_trajectory =
      make_uninitialized_fcl_spline_motion();
  std::shared_ptr<internal::StaticMotion> motion_region =
      std::make_shared<internal::StaticMotion>(region.pose);

  const fcl::ContinuousCollisionRequest request = make_fcl_request();

  bool collision_detected = false;

  for(auto it = begin_it; it != end_it; ++it)
  {
    const Trajectory::ConstProfilePtr profile = it->get_profile();

    Spline spline_trajectory{it};

    const Time spline_start_time =
        std::max(spline_trajectory.start_time(), start_time);
    const Time spline_finish_time =
        std::min(spline_trajectory.finish_time(), finish_time);

    *motion_trajectory = spline_trajectory.to_fcl(
          spline_start_time, spline_finish_time);

    assert(profile->get_shape());
    const auto obj_trajectory = fcl::ContinuousCollisionObject(
          geometry::FinalConvexShape::Implementation::get_collision(
            *profile->get_shape()), motion_trajectory);

    assert(region.shape);
    const auto& region_shapes = geometry::FinalShape::Implementation
        ::get_collisions(*region.shape);
    for(const auto& region_shape : region_shapes)
    {
      const auto obj_region = fcl::ContinuousCollisionObject(
            region_shape, motion_region);

      fcl::ContinuousCollisionResult result;
      fcl::collide(&obj_trajectory, &obj_region, request, result);
      if(result.is_collide)
      {
        if(output_iterators)
        {
          output_iterators->push_back(it);
          collision_detected = true;
        }
        else
        {
          return true;
        }
      }
    }
  }

  return collision_detected;
}
} // namespace internal

} // namespace rmf_traffic
