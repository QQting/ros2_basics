// Copyright 2020
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/* Author: Fernando González fergonzaramos@yahoo.es */

#include <memory>
#include <tf2/convert.h>
#include <tf2/transform_datatypes.h>
#include <tf2_sensor_msgs/tf2_sensor_msgs.h>
#include <tf2_ros/buffer.h>
#include <tf2_ros/transform_listener.h>
#include "tf2_geometry_msgs/tf2_geometry_msgs.h"
#include <geometry_msgs/msg/transform_stamped.hpp>
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <rclcpp/rclcpp.hpp>
#include "rclcpp_action/rclcpp_action.hpp"
#include <nav2_msgs/action/navigate_to_pose.hpp>
#include "rclcpp/time.hpp"
#include <string>
#include <vector>
#include "gb_visual_detection_3d_msgs/msg/bounding_boxes3d.hpp"
#include "gb_visual_detection_3d_msgs/msg/bounding_box3d.hpp"

#define HZ 5

using namespace std::chrono_literals;
using std::placeholders::_1;
using std::placeholders::_2;
using GoalHandleNav2= rclcpp_action::ClientGoalHandle
  <nav2_msgs::action::NavigateToPose>;

class Bboxes3d2nav2 : public rclcpp::Node
{
public:
  Bboxes3d2nav2(const std::string & node_name)
  : Node(node_name), tf2_buffer_(get_clock()),
  tf2_listener_(tf2_buffer_, true),
  person_saw_(false), bboxes_received_(false)
  {
    initParams();

    yolact_sub_ = this->create_subscription
      <gb_visual_detection_3d_msgs::msg::BoundingBoxes3d>(
      yolact_topic_, 1, std::bind(&Bboxes3d2nav2::bboxesCallback, this, _1));

    goal_update_pub_ = this->create_publisher<geometry_msgs::msg::PoseStamped>(
      goal_update_topic_, 1);
  }

  void
  step()
  {
    bool person_found;
    gb_visual_detection_3d_msgs::msg::BoundingBox3d bbox_detected;

    if (newPerson(person_found, bbox_detected)) {
      person_saw_ = true;
      sendActionGoal(bbox_detected);
    } else {
      if (!person_found) {
        person_saw_ = false;
        return;
      }
      if (bboxes_received_) {
        upadateGoal(bbox_detected);
      }
    }
    RCLCPP_INFO(get_logger(), "%s\n", "--------------------");
    bboxes_received_ = false;
  }

  void
  callServer()
  {
    goal_action_client_ = rclcpp_action::create_client
      <nav2_msgs::action::NavigateToPose>(shared_from_this(), "navigate_to_pose");
    waitForServer();
  }

private:
  void
  waitForServer()
  {
    if (!goal_action_client_->wait_for_action_server(std::chrono::seconds(2))) {
      RCLCPP_ERROR(get_logger(), "Nav2 action server not available");
      return;
    }
    RCLCPP_INFO(get_logger(), "%s\n", "Nav2 action server available");

    send_goal_options_ = rclcpp_action::Client<nav2_msgs::action::NavigateToPose>::SendGoalOptions();

    send_goal_options_.feedback_callback = std::bind(&Bboxes3d2nav2::feedbackCb, this, _1, _2);
    send_goal_options_.result_callback = std::bind(&Bboxes3d2nav2::resultCb, this, _1);
  }

  void
  upadateGoal(const gb_visual_detection_3d_msgs::msg::BoundingBox3d & bbox)
  {
    geometry_msgs::msg::PoseStamped pose_stamped;

    getPoseStamped(bbox, pose_stamped);

    goal_update_pub_->publish(pose_stamped);

    RCLCPP_INFO(get_logger(), "%s\n", "Update goal!");
  }

  void
  sendActionGoal(const gb_visual_detection_3d_msgs::msg::BoundingBox3d & bbox)
  {
    geometry_msgs::msg::PoseStamped pose_stamped;

    getPoseStamped(bbox, pose_stamped);

    // Compose action:

    auto goal_msg = nav2_msgs::action::NavigateToPose::Goal();
    goal_msg.pose = pose_stamped;
    goal_msg.behavior_tree = "MainTree";

    // Send Action:

    future_navigation_goal_handle_ = goal_action_client_->async_send_goal(
      goal_msg, send_goal_options_);

    goal_action_client_->async_send_goal(goal_msg);
    /*
    if (!future_navigation_goal_handle_.get()) {
      RCLCPP_ERROR(get_logger(), "Goal was rejected by server");
      return;
    }
    */

    RCLCPP_INFO(get_logger(), "%s\n", "Send action goal!");
  }

  void
  getPoseStamped(const gb_visual_detection_3d_msgs::msg::BoundingBox3d & bbox,
    geometry_msgs::msg::PoseStamped & pose_stamped)
  {
    double orig_x, orig_y, orig_z;
    geometry_msgs::msg::TransformStamped transform;
    geometry_msgs::msg::PoseStamped pose_stamped_in;

    // Get the object pose at its original frame:

    orig_x = (bbox.xmin + bbox.xmax) / 2.0;
    orig_y = (bbox.ymin + bbox.ymax) / 2.0;
    orig_z = 0.0;

    // Compose PoseStamped:

    pose_stamped_in.header = bboxes_header_;
    pose_stamped_in.header.stamp = now();
    pose_stamped_in.pose.position.x = orig_x;
    pose_stamped_in.pose.position.y = orig_y;
    pose_stamped_in.pose.position.z = orig_z;

    pose_stamped_in.pose.orientation.x = 0.0;
    pose_stamped_in.pose.orientation.y = 0.0;
    pose_stamped_in.pose.orientation.z = 0.0;
    pose_stamped_in.pose.orientation.w = 1.0;

    // Transform PoseStamped:

    try {
      transform = tf2_buffer_.lookupTransform(static_frame_, bboxes_header_.frame_id,
        tf2::TimePointZero);
    } catch (tf2::TransformException & ex) {
      RCLCPP_ERROR(this->get_logger(), "Transform error of sensor data: %s, %s\n",
        ex.what(), "quitting callback");
        person_saw_ = false;
      return;
    }

    tf2::doTransform<geometry_msgs::msg::PoseStamped>(
    pose_stamped_in, pose_stamped, transform);

    pose_stamped.pose.position.z = orig_z;

  }

  bool
  newPerson(bool & found, gb_visual_detection_3d_msgs::msg::BoundingBox3d & out_bbox)
  {
    found = false;
    for (auto bbox : bboxes_) {
      if (bbox.object_name == "person") {
        found = true;
        out_bbox = bbox;
        if (!person_saw_) {
          return true;
        }
      }
    }
    return false;
  }

  void
  feedbackCb(
    GoalHandleNav2::SharedPtr,
    const std::shared_ptr<const nav2_msgs::action::NavigateToPose::Feedback> feedback)
  {
    RCLCPP_INFO(get_logger(), "[%f] meters to Goal", feedback->distance_remaining);
  }

  void
  resultCb(const GoalHandleNav2::WrappedResult & result)
  {
    switch (result.code) {
      case rclcpp_action::ResultCode::SUCCEEDED:
        RCLCPP_WARN(get_logger(), "Goal Reached!");
        person_saw_ = false;
        break;
      case rclcpp_action::ResultCode::ABORTED:
        RCLCPP_ERROR(get_logger(), "Goal was aborted");
        return;
      case rclcpp_action::ResultCode::CANCELED:
        RCLCPP_ERROR(get_logger(), "Goal was canceled");
        return;
      default:
        RCLCPP_ERROR(get_logger(), "Unknown result code");
        return;
    }
  }

  void
  bboxesCallback(const gb_visual_detection_3d_msgs::msg::BoundingBoxes3d::SharedPtr msg)
  {
    // Save the fields of the message

    bboxes_header_ = msg->header;
    bboxes_ = msg->bounding_boxes;
    bboxes_received_ = true;
  }

  void
  initParams()
  {
    yolact_topic_ = "/darknet_ros_3d/bounding_boxes";
    static_frame_ = "map";
    goal_update_topic_ = "/goal_update";
  }

  rclcpp::Subscription
  <gb_visual_detection_3d_msgs::msg::BoundingBoxes3d>::SharedPtr yolact_sub_;

  rclcpp::Publisher<geometry_msgs::msg::PoseStamped>::SharedPtr goal_update_pub_;
  rclcpp_action::Client
    <nav2_msgs::action::NavigateToPose>::SharedPtr goal_action_client_;

  rclcpp_action::Client
    <nav2_msgs::action::NavigateToPose>::SendGoalOptions send_goal_options_;

  std::shared_future<GoalHandleNav2::SharedPtr> future_navigation_goal_handle_;

  tf2_ros::Buffer tf2_buffer_;
  tf2_ros::TransformListener tf2_listener_;

  std::string yolact_topic_, goal_update_topic_, static_frame_;
  std_msgs::msg::Header bboxes_header_;
  std::vector<gb_visual_detection_3d_msgs::msg::BoundingBox3d> bboxes_;
  bool person_saw_, bboxes_received_;
};

int
main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);

  auto node = std::make_shared<Bboxes3d2nav2>("bboxes3d_to_nav2_node");

  node->callServer();

  rclcpp::Rate loop_rate(HZ);
  while (rclcpp::ok()) {
    rclcpp::spin_some(node->get_node_base_interface());
    node->step();
    loop_rate.sleep();
  }

  rclcpp::shutdown();

  exit(EXIT_SUCCESS);
}
