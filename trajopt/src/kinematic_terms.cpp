#include <trajopt_utils/macros.h>
TRAJOPT_IGNORE_WARNINGS_PUSH
#include <Eigen/Geometry>
#include <Eigen/Core>
#include <boost/format.hpp>
#include <iostream>
#include <tesseract_kinematics/core/utils.h>
TRAJOPT_IGNORE_WARNINGS_POP

#include <trajopt/kinematic_terms.hpp>
#include <trajopt/utils.hpp>
#include <trajopt_sco/expr_ops.hpp>
#include <trajopt_sco/modeling_utils.hpp>
#include <trajopt_utils/eigen_conversions.hpp>
#include <trajopt_utils/eigen_slicing.hpp>
#include <trajopt_utils/logging.hpp>
#include <trajopt_utils/stl_to_string.hpp>

using namespace std;
using namespace sco;
using namespace Eigen;
using namespace util;

namespace
{
#if 0
Vector3d rotVec(const Matrix3d& m) {
  Quaterniond q; q = m;
  return Vector3d(q.x(), q.y(), q.z());
}
#endif

#if 0
VectorXd concat(const VectorXd& a, const VectorXd& b) {
  VectorXd out(a.size()+b.size());
  out.topRows(a.size()) = a;
  out.middleRows(a.size(), b.size()) = b;
  return out;
}

template <typename T>
vector<T> concat(const vector<T>& a, const vector<T>& b) {
  vector<T> out;
  vector<int> x;
  out.insert(out.end(), a.begin(), a.end());
  out.insert(out.end(), b.begin(), b.end());
  return out;
}
#endif
}  // namespace

namespace trajopt
{
VectorXd DynamicCartPoseErrCalculator::operator()(const VectorXd& dof_vals) const
{
  Isometry3d new_pose, target_pose;
  manip_->calcFwdKin(new_pose, dof_vals, kin_link_->link_name);
  manip_->calcFwdKin(target_pose, dof_vals, kin_target_->link_name);

  Eigen::Isometry3d link_tf = world_to_base_ * new_pose * kin_link_->transform * tcp_;
  Eigen::Isometry3d target_tf = world_to_base_ * target_pose * kin_target_->transform * target_tcp_;

  Eigen::VectorXd err = calcTransformError(target_tf, link_tf);
  Eigen::VectorXd reduced_err(indices_.size());
  for (int i = 0; i < indices_.size(); ++i)
    reduced_err[i] = err[indices_[i]];

  return reduced_err;  // This is available in 3.4 err(indices_, Eigen::all);
}

void DynamicCartPoseErrCalculator::Plot(const tesseract_visualization::Visualization::Ptr& plotter,
                                        const VectorXd& dof_vals)
{
  Isometry3d cur_pose, target_pose;

  manip_->calcFwdKin(cur_pose, dof_vals, kin_link_->link_name);
  manip_->calcFwdKin(target_pose, dof_vals, kin_target_->link_name);

  Eigen::Isometry3d cur_tf = world_to_base_ * cur_pose * kin_link_->transform * tcp_;
  Eigen::Isometry3d target_tf = world_to_base_ * target_pose * kin_target_->transform * target_tcp_;

  plotter->plotAxis(cur_tf, 0.05);
  plotter->plotAxis(target_tf, 0.05);
  plotter->plotArrow(cur_tf.translation(), target_tf.translation(), Eigen::Vector4d(1, 0, 1, 1), 0.005);
}

MatrixXd DynamicCartPoseJacCalculator::operator()(const VectorXd& dof_vals) const
{
  int n_dof = static_cast<int>(manip_->numJoints());
  MatrixXd jac_link(6, n_dof), jac_target(6, n_dof), jac0(6, n_dof);

  Isometry3d cur_pose, target_pose;

  manip_->calcFwdKin(cur_pose, dof_vals, kin_link_->link_name);
  manip_->calcFwdKin(target_pose, dof_vals, kin_target_->link_name);

  Eigen::Isometry3d cur_tf = world_to_base_ * cur_pose * kin_link_->transform * tcp_;
  Eigen::Isometry3d target_tf = world_to_base_ * target_pose * kin_target_->transform * target_tcp_;

  // Get the jacobian of link in the targets coordinate system
  manip_->calcJacobian(jac_link, dof_vals, kin_link_->link_name);
  tesseract_kinematics::jacobianChangeBase(jac_link, world_to_base_);
  tesseract_kinematics::jacobianChangeRefPoint(
      jac_link, (world_to_base_ * cur_pose).linear() * (kin_link_->transform * tcp_).translation());
  tesseract_kinematics::jacobianChangeBase(jac_link, target_tf.inverse());

  // Get the jacobian of the target in the targets coordinate system
  manip_->calcJacobian(jac_target, dof_vals, kin_target_->link_name);
  tesseract_kinematics::jacobianChangeBase(jac_target, world_to_base_);
  tesseract_kinematics::jacobianChangeRefPoint(
      jac_target, (world_to_base_ * target_pose).linear() * (kin_target_->transform * target_tcp_).translation());
  tesseract_kinematics::jacobianChangeBase(jac_target, target_tf.inverse());
  tesseract_kinematics::jacobianChangeRefPoint(jac_target, (target_tf.inverse() * cur_tf).translation());

  jac0 = jac_link - jac_target;

  // Paper:
  // https://ethz.ch/content/dam/ethz/special-interest/mavt/robotics-n-intelligent-systems/rsl-dam/documents/RobotDynamics2016/RD2016script.pdf
  // The jacobian of the robot is the geometric jacobian (Je) which maps generalized velocities in
  // joint space to time derivatives of the end-effector configuration representation. It does not
  // represent the analytic jacobian (Ja) given by a partial differentiation of position and rotation
  // to generalized coordinates. Since the geometric jacobian is unique there exists a linear mapping
  // between velocities and the derivatives of the representation.
  //
  // The approach in the paper was tried but it was having issues with getting correct jacobian.
  // Must of had an error in the implementation so should revisit at another time but the approach
  // below should be sufficient and faster than numerical calculations using the err function.

  // The approach below leverages the geometric jacobian and a small step in time to approximate
  // the partial derivative of the error function. Note that the rotational portion is the only part
  // that is required to be modified per the paper.
  Isometry3d pose_err = target_tf.inverse() * cur_tf;
  Eigen::Vector3d rot_err = calcRotationalError(pose_err.rotation());
  for (int c = 0; c < jac0.cols(); ++c)
  {
    auto new_pose_err = addTwist(pose_err, jac0.col(c), 1e-5);
    Eigen::VectorXd new_rot_err = calcRotationalError(new_pose_err.rotation());
    jac0.col(c).tail(3) = ((new_rot_err - rot_err) / 1e-5);
  }

  MatrixXd reduced_jac(indices_.size(), n_dof);
  for (int i = 0; i < indices_.size(); ++i)
    reduced_jac.row(i) = jac0.row(indices_[i]);

  return reduced_jac;  // This is available in 3.4 jac0(indices_, Eigen::all);
}

VectorXd CartPoseErrCalculator::operator()(const VectorXd& dof_vals) const
{
  Isometry3d new_pose;
  manip_->calcFwdKin(new_pose, dof_vals, kin_link_->link_name);

  new_pose = world_to_base_ * new_pose * kin_link_->transform * tcp_;

  Isometry3d pose_err = pose_inv_ * new_pose;
  Eigen::VectorXd err = concat(pose_err.translation(), calcRotationalError(pose_err.rotation()));
  Eigen::VectorXd reduced_err(indices_.size());
  for (int i = 0; i < indices_.size(); ++i)
    reduced_err[i] = err[indices_[i]];

  return reduced_err;  // This is available in 3.4 err(indices_, Eigen::all);
}

void CartPoseErrCalculator::Plot(const tesseract_visualization::Visualization::Ptr& plotter, const VectorXd& dof_vals)
{
  Isometry3d cur_pose;
  manip_->calcFwdKin(cur_pose, dof_vals, kin_link_->link_name);

  cur_pose = world_to_base_ * cur_pose * kin_link_->transform * tcp_;

  Isometry3d target = pose_inv_.inverse();

  plotter->plotAxis(cur_pose, 0.05);
  plotter->plotAxis(target, 0.05);
  plotter->plotArrow(cur_pose.translation(), target.translation(), Eigen::Vector4d(1, 0, 1, 1), 0.005);
}

MatrixXd CartPoseJacCalculator::operator()(const VectorXd& dof_vals) const
{
  int n_dof = static_cast<int>(manip_->numJoints());
  MatrixXd jac0(6, n_dof);
  Eigen::Isometry3d tf0;

  manip_->calcFwdKin(tf0, dof_vals, kin_link_->link_name);
  manip_->calcJacobian(jac0, dof_vals, kin_link_->link_name);
  tesseract_kinematics::jacobianChangeBase(jac0, world_to_base_);
  tesseract_kinematics::jacobianChangeRefPoint(
      jac0, (world_to_base_ * tf0).linear() * (kin_link_->transform * tcp_).translation());
  tesseract_kinematics::jacobianChangeBase(jac0, pose_inv_);

  // Paper:
  // https://ethz.ch/content/dam/ethz/special-interest/mavt/robotics-n-intelligent-systems/rsl-dam/documents/RobotDynamics2016/RD2016script.pdf
  // The jacobian of the robot is the geometric jacobian (Je) which maps generalized velocities in
  // joint space to time derivatives of the end-effector configuration representation. It does not
  // represent the analytic jacobian (Ja) given by a partial differentiation of position and rotation
  // to generalized coordinates. Since the geometric jacobian is unique there exists a linear mapping
  // between velocities and the derivatives of the representation.
  //
  // The approach in the paper was tried but it was having issues with getting correct jacobian.
  // Must of had an error in the implementation so should revisit at another time but the approach
  // below should be sufficient and faster than numerical calculations using the err function.

  // The approach below leverages the geometric jacobian and a small step in time to approximate
  // the partial derivative of the error function. Note that the rotational portion is the only part
  // that is required to be modified per the paper.
  Isometry3d pose_err = pose_inv_ * tf0;
  Eigen::Vector3d rot_err = calcRotationalError(pose_err.rotation());
  for (int c = 0; c < jac0.cols(); ++c)
  {
    auto new_pose_err = addTwist(pose_err, jac0.col(c), 1e-5);
    Eigen::VectorXd new_rot_err = calcRotationalError(new_pose_err.rotation());
    jac0.col(c).tail(3) = ((new_rot_err - rot_err) / 1e-5);
  }

  MatrixXd reduced_jac(indices_.size(), n_dof);
  for (int i = 0; i < indices_.size(); ++i)
    reduced_jac.row(i) = jac0.row(indices_[i]);

  return reduced_jac;  // This is available in 3.4 jac0(indices_, Eigen::all);
}

MatrixXd CartVelJacCalculator::operator()(const VectorXd& dof_vals) const
{
  int n_dof = static_cast<int>(manip_->numJoints());
  MatrixXd out(6, 2 * n_dof);

  MatrixXd jac0, jac1;
  Eigen::Isometry3d tf0, tf1;

  jac0.resize(6, manip_->numJoints());
  jac1.resize(6, manip_->numJoints());

  if (tcp_.translation().isZero())
  {
    manip_->calcFwdKin(tf0, dof_vals.topRows(n_dof), kin_link_->link_name);
    manip_->calcJacobian(jac0, dof_vals.topRows(n_dof), kin_link_->link_name);
    tesseract_kinematics::jacobianChangeBase(jac0, world_to_base_);
    tesseract_kinematics::jacobianChangeRefPoint(jac0,
                                                 (world_to_base_ * tf0).linear() * kin_link_->transform.translation());

    manip_->calcFwdKin(tf1, dof_vals.bottomRows(n_dof), kin_link_->link_name);
    manip_->calcJacobian(jac1, dof_vals.bottomRows(n_dof), kin_link_->link_name);
    tesseract_kinematics::jacobianChangeBase(jac1, world_to_base_);
    tesseract_kinematics::jacobianChangeRefPoint(jac1,
                                                 (world_to_base_ * tf1).linear() * kin_link_->transform.translation());
  }
  else
  {
    manip_->calcFwdKin(tf0, dof_vals.topRows(n_dof), kin_link_->link_name);
    manip_->calcJacobian(jac0, dof_vals.topRows(n_dof), kin_link_->link_name);
    tesseract_kinematics::jacobianChangeBase(jac0, world_to_base_);
    tesseract_kinematics::jacobianChangeRefPoint(
        jac0, (world_to_base_ * tf0).linear() * (kin_link_->transform * tcp_).translation());

    manip_->calcFwdKin(tf1, dof_vals.bottomRows(n_dof), kin_link_->link_name);
    manip_->calcJacobian(jac1, dof_vals.bottomRows(n_dof), kin_link_->link_name);
    tesseract_kinematics::jacobianChangeBase(jac1, world_to_base_);
    tesseract_kinematics::jacobianChangeRefPoint(
        jac1, (world_to_base_ * tf1).linear() * (kin_link_->transform * tcp_).translation());
  }

  out.block(0, 0, 3, n_dof) = -jac0.topRows(3);
  out.block(0, n_dof, 3, n_dof) = jac1.topRows(3);
  out.block(3, 0, 3, n_dof) = jac0.topRows(3);
  out.block(3, n_dof, 3, n_dof) = -jac1.topRows(3);
  return out;
}

VectorXd CartVelErrCalculator::operator()(const VectorXd& dof_vals) const
{
  int n_dof = static_cast<int>(manip_->numJoints());
  Isometry3d pose0, pose1;

  manip_->calcFwdKin(pose0, dof_vals.topRows(n_dof), kin_link_->link_name);
  manip_->calcFwdKin(pose1, dof_vals.bottomRows(n_dof), kin_link_->link_name);

  pose0 = world_to_base_ * pose0 * kin_link_->transform * tcp_;
  pose1 = world_to_base_ * pose1 * kin_link_->transform * tcp_;

  VectorXd out(6);
  out.topRows(3) = (pose1.translation() - pose0.translation() - Vector3d(limit_, limit_, limit_));
  out.bottomRows(3) = (pose0.translation() - pose1.translation() - Vector3d(limit_, limit_, limit_));
  return out;
}

Eigen::VectorXd JointVelErrCalculator::operator()(const VectorXd& var_vals) const
{
  assert(var_vals.rows() % 2 == 0);
  // Top half of the vector are the joint values. The bottom half are the 1/dt values
  int half = static_cast<int>(var_vals.rows() / 2);
  int num_vels = half - 1;
  // (x1-x0)*(1/dt)
  VectorXd vel = (var_vals.segment(1, num_vels) - var_vals.segment(0, num_vels)).array() *
                 var_vals.segment(half + 1, num_vels).array();

  // Note that for equality terms tols are 0, so error is effectively doubled
  VectorXd result(vel.rows() * 2);
  result.topRows(vel.rows()) = -(upper_tol_ - (vel.array() - target_));
  result.bottomRows(vel.rows()) = lower_tol_ - (vel.array() - target_);
  return result;
}

MatrixXd JointVelJacCalculator::operator()(const VectorXd& var_vals) const
{
  // var_vals = (theta_t1, theta_t2, theta_t3 ... 1/dt_1, 1/dt_2, 1/dt_3 ...)
  int num_vals = static_cast<int>(var_vals.rows());
  int half = num_vals / 2;
  int num_vels = half - 1;
  MatrixXd jac = MatrixXd::Zero(num_vels * 2, num_vals);

  for (int i = 0; i < num_vels; i++)
  {
    // v = (j_i+1 - j_i)*(1/dt)
    // We calculate v with the dt from the second pt
    int time_index = i + half + 1;
    // dv_i/dj_i = -(1/dt)
    jac(i, i) = -1.0 * var_vals(time_index);
    // dv_i/dj_i+1 = (1/dt)
    jac(i, i + 1) = 1.0 * var_vals(time_index);
    // dv_i/dt_i = j_i+1 - j_i
    jac(i, time_index) = var_vals(i + 1) - var_vals(i);
    // All others are 0
  }

  // bottom half is negative velocities
  jac.bottomRows(num_vels) = -jac.topRows(num_vels);

  return jac;
}

// TODO: convert to (1/dt) and use central finite difference method
VectorXd JointAccErrCalculator::operator()(const VectorXd& var_vals) const
{
  assert(var_vals.rows() % 2 == 0);
  int half = static_cast<int>(var_vals.rows() / 2);
  int num_acc = half - 2;
  VectorXd vels = vel_calc(var_vals);

  // v1-v0
  VectorXd vel_diff = (vels.segment(1, num_acc) - vels.segment(0, num_acc));
  // I'm not sure about this. We should probably use same method we use in non time version
  // v1-v0/avg(dt1,dt0)
  VectorXd acc =
      2.0 * vel_diff.array() / (var_vals.segment(half + 1, num_acc) + var_vals.segment(half + 2, num_acc)).array();

  return acc.array() - limit_;
}

MatrixXd JointAccJacCalculator::operator()(const VectorXd& var_vals) const
{
  int num_vals = static_cast<int>(var_vals.rows());
  int half = num_vals / 2;
  MatrixXd jac = MatrixXd::Zero(half - 2, num_vals);

  VectorXd vels = vel_calc(var_vals);
  MatrixXd vel_jac = vel_jac_calc(var_vals);
  for (int i = 0; i < jac.rows(); i++)
  {
    int dt_1_index = i + half + 1;
    int dt_2_index = dt_1_index + 1;
    double dt_1 = var_vals(dt_1_index);
    double dt_2 = var_vals(dt_2_index);
    double total_dt = dt_1 + dt_2;

    jac(i, i) = 2.0 * (vel_jac(i + 1, i) - vel_jac(i, i)) / total_dt;
    jac(i, i + 1) = 2.0 * (vel_jac(i + 1, i + 1) - vel_jac(i, i + 1)) / total_dt;
    jac(i, i + 2) = 2.0 * (vel_jac(i + 1, i + 2) - vel_jac(i, i + 2)) / total_dt;

    jac(i, dt_1_index) = 2.0 * ((vel_jac(i + 1, dt_1_index) - vel_jac(i, dt_1_index)) / total_dt -
                                (vels(i + 1) - vels(i)) / sq(total_dt));
    jac(i, dt_2_index) = 2.0 * ((vel_jac(i + 1, dt_2_index) - vel_jac(i, dt_2_index)) / total_dt -
                                (vels(i + 1) - vels(i)) / sq(total_dt));
  }

  return jac;
}

// TODO: convert to (1/dt) and use central finite difference method
VectorXd JointJerkErrCalculator::operator()(const VectorXd& var_vals) const
{
  assert(var_vals.rows() % 2 == 0);
  int half = static_cast<int>(var_vals.rows() / 2);
  int num_jerk = half - 3;
  VectorXd acc = acc_calc(var_vals);

  VectorXd acc_diff = acc.segment(1, num_jerk) - acc.segment(0, num_jerk);

  VectorXd jerk = 3.0 * acc_diff.array() /
                  (var_vals.segment(half + 1, num_jerk) + var_vals.segment(half + 2, num_jerk) +
                   var_vals.segment(half + 3, num_jerk))
                      .array();

  return jerk.array() - limit_;
}

MatrixXd JointJerkJacCalculator::operator()(const VectorXd& var_vals) const
{
  int num_vals = static_cast<int>(var_vals.rows());
  int half = num_vals / 2;
  MatrixXd jac = MatrixXd::Zero(half - 3, num_vals);

  VectorXd acc = acc_calc(var_vals);
  MatrixXd acc_jac = acc_jac_calc(var_vals);

  for (int i = 0; i < jac.rows(); i++)
  {
    int dt_1_index = i + half + 1;
    int dt_2_index = dt_1_index + 1;
    int dt_3_index = dt_2_index + 1;
    double dt_1 = var_vals(dt_1_index);
    double dt_2 = var_vals(dt_2_index);
    double dt_3 = var_vals(dt_3_index);
    double total_dt = dt_1 + dt_2 + dt_3;

    jac(i, i) = 3.0 * (acc_jac(i + 1, i) - acc_jac(i, i)) / total_dt;
    jac(i, i + 1) = 3.0 * (acc_jac(i + 1, i + 1) - acc_jac(i, i + 1)) / total_dt;
    jac(i, i + 2) = 3.0 * (acc_jac(i + 1, i + 2) - acc_jac(i, i + 2)) / total_dt;
    jac(i, i + 3) = 3.0 * (acc_jac(i + 1, i + 3) - acc_jac(i, i + 3)) / total_dt;

    jac(i, dt_1_index) =
        3.0 * ((acc_jac(i + 1, dt_1_index) - acc_jac(i, dt_1_index)) / total_dt - (acc(i + 1) - acc(i)) / sq(total_dt));
    jac(i, dt_2_index) =
        3.0 * ((acc_jac(i + 1, dt_2_index) - acc_jac(i, dt_2_index)) / total_dt - (acc(i + 1) - acc(i)) / sq(total_dt));
    jac(i, dt_3_index) =
        3.0 * ((acc_jac(i + 1, dt_3_index) - acc_jac(i, dt_3_index)) / total_dt - (acc(i + 1) - acc(i)) / sq(total_dt));
  }

  return jac;
}

VectorXd TimeCostCalculator::operator()(const VectorXd& time_vals) const
{
  VectorXd total(1);
  total(0) = time_vals.cwiseInverse().sum() - limit_;
  return total;
}

MatrixXd TimeCostJacCalculator::operator()(const VectorXd& time_vals) const
{
  MatrixXd jac(1, time_vals.rows());
  jac.row(0) = -1 * time_vals.cwiseAbs2().cwiseInverse();
  return jac;
}

}  // namespace trajopt
