/*******************************************************************************
 * Copyright (c) 2016, ROBOTIS CO., LTD.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of ROBOTIS nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *******************************************************************************/

/* Author: Kayman Jung */

#include <stdio.h>
#include "op3_direct_control_module/direct_control_module.h"

namespace robotis_op
{

DirectControlModule::DirectControlModule()
  : control_cycle_msec_(0),
    stop_process_(false),
    is_moving_(false),
    is_updated_(false),
    is_blocked_(false),
    r_min_diff_(0.07),
    l_min_diff_(0.07),
    tra_count_(0),
    tra_size_(0),
    default_moving_time_(1.0),
    default_moving_angle_(45),
    moving_time_(3.0),
    BASE_INDEX(0),
    HEAD_INDEX(20),
    RIGHT_END_EFFECTOR_INDEX(21),
    LEFT_END_EFFECTOR_INDEX(22),
    DEBUG(false)
{
  enable_ = false;
  module_name_ = "direct_control_module";
  control_mode_ = robotis_framework::PositionControl;

  //  result_["head_pan"] = new robotis_framework::DynamixelState();
  //  result_["head_tilt"] = new robotis_framework::DynamixelState();

  //  using_joint_name_["head_pan"] = 0;
  //  using_joint_name_["head_tilt"] = 1;

  //  max_angle_[using_joint_name_["head_pan"]] = 85 * DEGREE2RADIAN;
  //  min_angle_[using_joint_name_["head_pan"]] = -85 * DEGREE2RADIAN;
  //  max_angle_[using_joint_name_["head_tilt"]] = 30 * DEGREE2RADIAN;
  //  min_angle_[using_joint_name_["head_tilt"]] = -75 * DEGREE2RADIAN;

  //  target_position_ = Eigen::MatrixXd::Zero(1, result_.size());
  //  current_position_ = Eigen::MatrixXd::Zero(1, result_.size());
  //  goal_position_ = Eigen::MatrixXd::Zero(1, result_.size());
  //  goal_velocity_ = Eigen::MatrixXd::Zero(1, result_.size());
  //  goal_acceleration_ = Eigen::MatrixXd::Zero(1, result_.size());

  last_msg_time_ = ros::Time::now();
}

DirectControlModule::~DirectControlModule()
{
  queue_thread_.join();
}

void DirectControlModule::initialize(const int control_cycle_msec, robotis_framework::Robot *robot)
{
  op3_kinematics_ = new OP3KinematicsDynamics(WholeBody);

  // init result, joint_id_table
  int joint_index = 0;
  for (std::map<std::string, robotis_framework::Dynamixel*>::iterator it = robot->dxls_.begin();
       it != robot->dxls_.end(); it++)
  {
    std::string joint_name = it->first;
    robotis_framework::Dynamixel* dxl_info = it->second;

    result_[joint_name] = new robotis_framework::DynamixelState();
    result_[joint_name]->goal_position_ = dxl_info->dxl_state_->goal_position_;

    using_joint_name_[joint_name] = joint_index++;
  }

  target_position_ = Eigen::MatrixXd::Zero(1, result_.size());
  present_position_ = Eigen::MatrixXd::Zero(1, result_.size());
  goal_position_ = Eigen::MatrixXd::Zero(1, result_.size());
  goal_velocity_ = Eigen::MatrixXd::Zero(1, result_.size());
  goal_acceleration_ = Eigen::MatrixXd::Zero(1, result_.size());


  // setting of queue thread
  queue_thread_ = boost::thread(boost::bind(&DirectControlModule::queueThread, this));

  control_cycle_msec_ = control_cycle_msec;

  ros::NodeHandle ros_node;

  /* get Param */
  ros_node.param<double>("/robotis/direct_control/default_moving_time", default_moving_time_, default_moving_time_);
  ros_node.param<double>("/robotis/direct_control/default_moving_angle", default_moving_angle_, default_moving_angle_);

  /* publish topics */
  status_msg_pub_ = ros_node.advertise<robotis_controller_msgs::StatusMsg>("/robotis/status", 0);
}

void DirectControlModule::queueThread()
{
  ros::NodeHandle ros_node;
  ros::CallbackQueue callback_queue;

  ros_node.setCallbackQueue(&callback_queue);

  /* subscribe topics */
  ros::Subscriber set_head_joint_sub = ros_node.subscribe("/robotis/direct_control/set_joint_states", 1,
                                                          &DirectControlModule::setJointCallback, this);

  ros::WallDuration duration(control_cycle_msec_ / 1000.0);
  while(ros_node.ok())
    callback_queue.callAvailable(duration);
}

void DirectControlModule::setJointCallback(const sensor_msgs::JointState::ConstPtr &msg)
{
  if (enable_ == false)
  {
    ROS_INFO_THROTTLE(1, "Direct control module is not enable.");
    publishStatusMsg(robotis_controller_msgs::StatusMsg::STATUS_ERROR, "Not Enable");
    return;
  }

  int sleep_count = 0;
  while(is_updated_ == false)
  {
    usleep(control_cycle_msec_ * 1000);
    if(++sleep_count > 100)
    {
      ROS_ERROR("present joint angle is not updated");
      return;
    }
  }

  // moving time
  moving_time_ = 0.5;               // default : 1 sec

  // set target joint angle
  target_position_ = goal_position_;        // default

  for (int ix = 0; ix < msg->name.size(); ix++)
  {
    std::string joint_name = msg->name[ix];
    std::map<std::string, int>::iterator joint_it = using_joint_name_.find(joint_name);

    if (joint_it != using_joint_name_.end())
    {
      double target_position = 0.0;
      int joint_index = joint_it->second;

      // set target position
      target_position = msg->position[ix];

      // check angle limit

      // apply target position
      target_position_.coeffRef(0, joint_index) = target_position;

      // set time
      double angle_unit = 20 * M_PI / 180;
      double calc_moving_time = fabs(goal_position_.coeff(0, joint_index) - target_position_.coeff(0, joint_index))
          / angle_unit;
      if (calc_moving_time > moving_time_)
        moving_time_ = calc_moving_time;

      if (DEBUG)
        std::cout << "joint : " << joint_name << ", Index : " << joint_index << ", Angle : " << msg->position[ix]
                     << ", Time : " << moving_time_ << std::endl;
    }
  }

  // set init joint vel, accel
  goal_velocity_ = Eigen::MatrixXd::Zero(1, result_.size());
  goal_acceleration_ = Eigen::MatrixXd::Zero(1, result_.size());

  if (is_moving_ == true && is_blocked_ == false)
  {
    goal_velocity_ = calc_joint_vel_tra_.block(tra_count_, 0, 1, result_.size());
    goal_acceleration_ = calc_joint_accel_tra_.block(tra_count_, 0, 1, result_.size());
  }

  // generate trajectory
  tra_gene_thread_ = new boost::thread(boost::bind(&DirectControlModule::jointTraGeneThread, this));
  delete tra_gene_thread_;
}

void DirectControlModule::process(std::map<std::string, robotis_framework::Dynamixel *> dxls,
                                  std::map<std::string, double> sensors)
{
  if (enable_ == false)
    return;

  tra_lock_.lock();

  // get joint data from robot
  for (std::map<std::string, robotis_framework::DynamixelState *>::iterator state_it = result_.begin();
       state_it != result_.end(); state_it++)
  {
    std::string joint_name = state_it->first;
    int index = using_joint_name_[joint_name];

    robotis_framework::Dynamixel *_dxl = NULL;
    std::map<std::string, robotis_framework::Dynamixel*>::iterator dxl_it = dxls.find(joint_name);
    if (dxl_it != dxls.end())
      _dxl = dxl_it->second;
    else
      continue;

    present_position_.coeffRef(0, index) = _dxl->dxl_state_->present_position_;
    goal_position_.coeffRef(0, index) = _dxl->dxl_state_->goal_position_;
    result_[joint_name]->goal_position_ = _dxl->dxl_state_->goal_position_;
  }

  is_updated_ = true;

  // check to stop
  if (stop_process_ == true)
  {
    stopMoving();
  }
  else
  {
    // process
    if (tra_size_ != 0)
    {
      // start of steps
      if (tra_count_ == 0)
      {
        startMoving();
      }

      // end of steps
      if (tra_count_ >= tra_size_)
      {
        finishMoving();
      }
      else
      {
        // update goal position
        goal_position_ = calc_joint_tra_.block(tra_count_, 0, 1, result_.size());
        tra_count_ += 1;
      }
    }
  }
  tra_lock_.unlock();

  // set goal angle and run forward kinematics
  for (std::map<std::string, robotis_framework::DynamixelState *>::iterator state_it = result_.begin();
       state_it != result_.end(); state_it++)
  {
    std::string joint_name = state_it->first;
    int index = using_joint_name_[joint_name];
    double goal_position = goal_position_.coeff(0, index);

    LinkData *op3_link = op3_kinematics_->getLinkData(joint_name);
    if(op3_link != NULL)
      op3_link->joint_angle_ = goal_position;
  }

  op3_kinematics_->calcForwardKinematics(0);

  // check self collision
  bool collision_result = checkSelfCollision();

  if(collision_result == true)
  {
    is_blocked_ = true;
    return;
  }
  else
    is_blocked_ = false;

  // set joint data to robot
  for (std::map<std::string, robotis_framework::DynamixelState *>::iterator state_it = result_.begin();
       state_it != result_.end(); state_it++)
  {
    std::string joint_name = state_it->first;
    int index = using_joint_name_[joint_name];
    double goal_position = goal_position_.coeff(0, index);

    result_[joint_name]->goal_position_ = goal_position;
  }
}

void DirectControlModule::stop()
{
  tra_lock_.lock();

  if (is_moving_ == true)
    stop_process_ = true;

  tra_lock_.unlock();

  return;
}

bool DirectControlModule::isRunning()
{
  return is_moving_;
}

void DirectControlModule::onModuleEnable()
{
  is_updated_ = false;
  is_blocked_ = false;
  r_min_diff_ = 0.07;
  l_min_diff_ = 0.07;
}

void DirectControlModule::onModuleDisable()
{

}

void DirectControlModule::startMoving()
{
  is_moving_ = true;

  // start procedure
}

void DirectControlModule::finishMoving()
{
  // init value
  calc_joint_tra_ = goal_position_;
  tra_size_ = 0;
  tra_count_ = 0;
  is_moving_ = false;

  // log
  publishStatusMsg(robotis_controller_msgs::StatusMsg::STATUS_INFO, "Head movement is finished.");

  if (DEBUG)
    std::cout << "Trajectory End" << std::endl;
}

void DirectControlModule::stopMoving()
{
  // init value
  calc_joint_tra_ = goal_position_;
  tra_size_ = 0;
  tra_count_ = 0;
  is_moving_ = false;
  stop_process_ = false;

  // log
  publishStatusMsg(robotis_controller_msgs::StatusMsg::STATUS_WARN, "Stop Module.");
}

/*
 simple minimum jerk trajectory

 pos_start : position at initial state
 vel_start : velocity at initial state
 accel_start : acceleration at initial state

 pos_end : position at final state
 vel_end : velocity at final state
 accel_end : acceleration at final state

 smp_time : sampling time

 mov_time : movement time
 */
Eigen::MatrixXd DirectControlModule::calcMinimumJerkTraPVA(double pos_start, double vel_start, double accel_start,
                                                           double pos_end, double vel_end, double accel_end,
                                                           double smp_time, double mov_time)
{
  Eigen::MatrixXd poly_matrix(3, 3);
  Eigen::MatrixXd poly_vector(3, 1);

  poly_matrix << robotis_framework::powDI(mov_time, 3), robotis_framework::powDI(mov_time, 4), robotis_framework::powDI(
        mov_time, 5), 3 * robotis_framework::powDI(mov_time, 2), 4 * robotis_framework::powDI(mov_time, 3), 5
      * robotis_framework::powDI(mov_time, 4), 6 * mov_time, 12 * robotis_framework::powDI(mov_time, 2), 20
      * robotis_framework::powDI(mov_time, 3);

  poly_vector << pos_end - pos_start - vel_start * mov_time - accel_start * pow(mov_time, 2) / 2, vel_end - vel_start
      - accel_start * mov_time, accel_end - accel_start;

  Eigen::MatrixXd poly_coeff = poly_matrix.inverse() * poly_vector;

  int all_time_steps = round(mov_time / smp_time + 1);

  Eigen::MatrixXd time = Eigen::MatrixXd::Zero(all_time_steps, 1);
  Eigen::MatrixXd minimum_jer_tra = Eigen::MatrixXd::Zero(all_time_steps, 3);

  for (int step = 0; step < all_time_steps; step++)
    time.coeffRef(step, 0) = step * smp_time;

  for (int step = 0; step < all_time_steps; step++)
  {
    // position
    minimum_jer_tra.coeffRef(step, 0) = pos_start + vel_start * time.coeff(step, 0)
        + 0.5 * accel_start * robotis_framework::powDI(time.coeff(step, 0), 2)
        + poly_coeff.coeff(0, 0) * robotis_framework::powDI(time.coeff(step, 0), 3)
        + poly_coeff.coeff(1, 0) * robotis_framework::powDI(time.coeff(step, 0), 4)
        + poly_coeff.coeff(2, 0) * robotis_framework::powDI(time.coeff(step, 0), 5);
    // velocity
    minimum_jer_tra.coeffRef(step, 1) = vel_start + accel_start * time.coeff(step, 0)
        + 3 * poly_coeff.coeff(0, 0) * robotis_framework::powDI(time.coeff(step, 0), 2)
        + 4 * poly_coeff.coeff(1, 0) * robotis_framework::powDI(time.coeff(step, 0), 3)
        + 5 * poly_coeff.coeff(2, 0) * robotis_framework::powDI(time.coeff(step, 0), 4);
    // accel
    minimum_jer_tra.coeffRef(step, 2) = accel_start + 6 * poly_coeff.coeff(0, 0) * time.coeff(step, 0)
        + 12 * poly_coeff.coeff(1, 0) * robotis_framework::powDI(time.coeff(step, 0), 2)
        + 20 * poly_coeff.coeff(2, 0) * robotis_framework::powDI(time.coeff(step, 0), 3);
  }

  return minimum_jer_tra;
}

bool DirectControlModule::checkSelfCollision()
{
  // right arm
  // get length between right arm and base
  double diff_length = 0.0;
  bool result = getDiff(RIGHT_END_EFFECTOR_INDEX, BASE_INDEX, diff_length);

  // check collision
  if(result == true && diff_length < 0.07)
  {
    if(r_min_diff_ < diff_length)
    {
      r_min_diff_ = diff_length;
      return false;
    }
    ROS_ERROR("Self Collision : RIGHT_ARM and BASE");
    r_min_diff_ = diff_length;
    return true;
  }

  // left arm
  // get left arm end effect position
  diff_length = 0.0;
  result = getDiff(LEFT_END_EFFECTOR_INDEX, BASE_INDEX, diff_length);

  // check collision
  if(result == true && diff_length < 0.07)
  {
    if(l_min_diff_ < diff_length)
    {
      l_min_diff_ = diff_length;
      return false;
    }
    ROS_ERROR("Self Collision : LEFT_ARM and BASE");
    l_min_diff_ = diff_length;
    return true;
  }

  return false;
}

bool DirectControlModule::getDiff(int end_index, int base_index, double &diff)
{
  if(op3_kinematics_->op3_link_data_[end_index] == NULL | op3_kinematics_->op3_link_data_[base_index] == NULL)
    return false;

  Eigen::Vector3d end_position = op3_kinematics_->op3_link_data_[end_index]->position_;
  Eigen::Vector3d base_position = op3_kinematics_->op3_link_data_[base_index]->position_;
  Eigen::Vector3d diff_vec = base_position - end_position;
  diff_vec.coeffRef(2) = 0;

  diff = diff_vec.norm();

  ROS_WARN_STREAM_COND(DEBUG, "\nBase Position [" << base_position.coeff(0) << ", " << base_position.coeff(1) << ", " << base_position.coeff(2) << "] \n"
                  << "End Position [" << end_position.coeff(0) << ", " << end_position.coeff(1) << ", " << end_position.coeff(2) << "] \n"
                  << "Diff : " << diff);

  return true;
}

void DirectControlModule::jointTraGeneThread()
{
  tra_lock_.lock();

  double smp_time = control_cycle_msec_ * 0.001;		// ms -> s
  int all_time_steps = int(moving_time_ / smp_time) + 1;

  // for debug
  try
  {
    calc_joint_tra_.resize(all_time_steps, result_.size());
    calc_joint_vel_tra_.resize(all_time_steps, result_.size());
    calc_joint_accel_tra_.resize(all_time_steps, result_.size());
  }
  catch(std::exception &e)
  {
    std::cout << "All step tile : " << all_time_steps << std::endl;
    std::cout << e.what() << std::endl;
    throw;
  }

  for (std::map<std::string, robotis_framework::DynamixelState *>::iterator state_it = result_.begin();
       state_it != result_.end(); state_it++)
  {
    std::string joint_name = state_it->first;
    int index = using_joint_name_[joint_name];

    double ini_pos = goal_position_.coeff(0, index);
    double ini_vel = goal_velocity_.coeff(0, index);
    double ini_accel = goal_acceleration_.coeff(0, index);

    double tar_value = target_position_.coeff(0, index);

    Eigen::MatrixXd tra = calcMinimumJerkTraPVA(ini_pos, ini_vel, ini_accel, tar_value, 0.0, 0.0, smp_time,
                                                moving_time_);

    calc_joint_tra_.block(0, index, all_time_steps, 1) = tra.block(0, Position, all_time_steps, 1);
    calc_joint_vel_tra_.block(0, index, all_time_steps, 1) = tra.block(0, Velocity, all_time_steps, 1);
    calc_joint_accel_tra_.block(0, index, all_time_steps, 1) = tra.block(0, Acceleration, all_time_steps, 1);
  }

  tra_size_ = calc_joint_tra_.rows();
  tra_count_ = 0;

  if (DEBUG)
    ROS_INFO("[ready] make trajectory : %d, %d", tra_size_, tra_count_);

  tra_lock_.unlock();
}

void DirectControlModule::publishStatusMsg(unsigned int type, std::string msg)
{
  ros::Time now = ros::Time::now();

  if(msg.compare(last_msg_) == 0)
  {
    ros::Duration dur = now - last_msg_time_;
    if(dur.sec < 1)
      return;
  }

  robotis_controller_msgs::StatusMsg status_msg;
  status_msg.header.stamp = now;
  status_msg.type = type;
  status_msg.module_name = "Direct Control";
  status_msg.status_msg = msg;

  status_msg_pub_.publish(status_msg);

  last_msg_ = msg;
  last_msg_time_ = now;
}
}
