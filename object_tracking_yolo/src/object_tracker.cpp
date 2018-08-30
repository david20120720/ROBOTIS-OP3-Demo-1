/*******************************************************************************
* Copyright 2017 ROBOTIS CO., LTD.
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
*******************************************************************************/

/* Author: Kayman Jung */

#include "object_tracking_yolo/object_tracker.h"

namespace robotis_op
{

ObjectTracker::ObjectTracker()
  : nh_(ros::this_node::getName()),
    FOV_WIDTH(35.2 * M_PI / 180),
    FOV_HEIGHT(21.6 * M_PI / 180),
    NOT_FOUND_THRESHOLD(50),
    WAITING_THRESHOLD(5),
    INIT_POSE_INDEX(1),
    INIT_PAN(0.0),
    INIT_TILT(0.0),
    use_head_scan_(true),
    count_not_found_(0),
    on_tracking_(false),
    object_current_pan_(0),
    object_current_tilt_(0),
    x_error_sum_(0),
    y_error_sum_(0),
    object_current_size_(0),
    tracking_status_(NotFound),
    object_start_command_("sports ball"),
    object_stop_command_("teddy bear"),
    object_speak_command_("banana"),
    object_target_("cell phone"),
    target_index_(0),
    is_ready_to_demo_(false),
    TIME_TO_BACK(10.0),
    DEBUG_PRINT(false)
{
  ros::NodeHandle param_nh("~");
  p_gain_ = param_nh.param("p_gain", 0.4);
  i_gain_ = param_nh.param("i_gain", 0.0);
  d_gain_ = param_nh.param("d_gain", 0.0);

  // get config
  std::string default_config_path = ros::package::getPath(ROS_PACKAGE_NAME) + "/config/config.yaml";
  config_path_ = param_nh.param("/config", default_config_path);
  getConfig(config_path_);

  ROS_INFO_STREAM("Object tracking Gain : " << p_gain_ << ", " << i_gain_ << ", " << d_gain_);

  set_module_pub_ = nh_.advertise<std_msgs::String>("/robotis/enable_ctrl_module", 0);
  head_offset_joint_pub_ = nh_.advertise<sensor_msgs::JointState>("/robotis/head_control/set_joint_states_offset", 0);
  head_joint_pub_ = nh_.advertise<sensor_msgs::JointState>("/robotis/head_control/set_joint_states", 0);
  led_pub_ = nh_.advertise<robotis_controller_msgs::SyncWriteItem>("/robotis/sync_write_item", 0);
  motion_index_pub_ = nh_.advertise<std_msgs::Int32>("/robotis/action/page_num", 0);
  //  head_scan_pub_ = nh_.advertise<std_msgs::String>("/robotis/head_control/scan_command", 0);
  //  error_pub_ = nh_.advertise<std_msgs::Float64MultiArray>("/ball_tracker/errors", 0);

  object_sub_ = nh_.subscribe("/darknet_ros/bounding_boxes", 1, &ObjectTracker::objectCallback, this);
  buttuon_sub_ = nh_.subscribe("/robotis/open_cr/button", 1, &ObjectTracker::buttonHandlerCallback, this);
  //  ball_tracking_command_sub_ = nh_.subscribe("/ball_tracker/command", 1, &ObjectTracker::ballTrackerCommandCallback, this);

  set_joint_module_client_ = nh_.serviceClient<robotis_controller_msgs::SetModule>("/robotis/set_present_ctrl_modules");

  test_sub_ = nh_.subscribe("/ros_command", 1, &ObjectTracker::getROSCommand, this);

  last_found_time_ = ros::Time::now();
  boost::thread timer_thread = boost::thread(boost::bind(&ObjectTracker::timerThread, this));
}

ObjectTracker::~ObjectTracker()
{

}

//void ObjectTracker::ballPositionCallback(const op3_ball_detector::CircleSetStamped::ConstPtr &msg)
//{
//  for (int idx = 0; idx < msg->circles.size(); idx++)
//  {
//    if (ball_position_.z >= msg->circles[idx].z)
//      continue;

//    ball_position_ = msg->circles[idx];
//  }
//}

//void ObjectTracker::ballTrackerCommandCallback(const std_msgs::String::ConstPtr &msg)
//{
//  if (msg->data == "start")
//  {
//    startTracking();
//  }
//  else if (msg->data == "stop")
//  {
//    stopTracking();
//  }
//  else if (msg->data == "toggle_start")
//  {
//    if (on_tracking_ == false)
//      startTracking();
//    else
//      stopTracking();
//  }
//}

void ObjectTracker::startTracking()
{
  // get config
  getConfig(config_path_);

  if(is_ready_to_demo_ == false)
    readyToDemo();

  on_tracking_ = true;
  ROS_INFO_STREAM("Start Object tracking - " << object_target_);
}

void ObjectTracker::stopTracking()
{
  on_tracking_ = false;
  ROS_INFO("Stop Object tracking");

  object_current_pan_ = 0;
  object_current_tilt_ = 0;
  x_error_sum_ = 0;
  y_error_sum_ = 0;
}

void ObjectTracker::setBeingScanning(bool use_scan)
{
  use_head_scan_ = use_scan;
}

int ObjectTracker::processTracking()
{
  int tracking_status = Found;

  if (on_tracking_ == false)
  {
    //    object_position_.z = 0;
    //    count_not_found_ = 0;
    return NotFound;
  }

  // check object position
  if (object_position_.z <= 0)
  {
    count_not_found_++;

    if (count_not_found_ < WAITING_THRESHOLD)
    {
      if(tracking_status_ == Found || tracking_status_ == Waiting)
        tracking_status = Waiting;
      else
        tracking_status = NotFound;
    }
    else if (count_not_found_ > NOT_FOUND_THRESHOLD)
    {
      //      scanBall();
      count_not_found_ = 0;
      tracking_status = NotFound;
    }
    else
    {
      tracking_status = NotFound;
    }
  }
  else
  {
    count_not_found_ = 0;
  }

  // if ball is found
  // convert ball position to desired angle(rad) of head
  // ball_position : top-left is (-1, -1), bottom-right is (+1, +1)
  // offset_rad : top-left(+, +), bottom-right(-, -)
  double x_error = 0.0, y_error = 0.0, ball_size = 0.0;

  // handle RGB LED
  if(tracking_status_ != tracking_status)
  {
    switch(tracking_status)
    {
      case ObjectTracker::Found:
        setRGBLED(0x1F, 0x1F, 0x1F);
        break;

      case ObjectTracker::NotFound:
        setRGBLED(0, 0, 0);
        break;

      default:
        break;
    }
  }

  switch (tracking_status)
  {
  case NotFound:
    tracking_status_ = tracking_status;
    object_current_pan_ = 0;
    object_current_tilt_ = 0;
    x_error_sum_ = 0;
    y_error_sum_ = 0;
    return tracking_status;

  case Waiting:
    tracking_status_ = tracking_status;
    return tracking_status;

  case Found:
    x_error = -atan(object_position_.x * tan(FOV_WIDTH));
    y_error = -atan(object_position_.y * tan(FOV_HEIGHT));
    ball_size = object_position_.z;
    break;

  default:
    break;
  }

  ROS_INFO_STREAM_COND(DEBUG_PRINT, "--------------------------------------------------------------");
  ROS_INFO_STREAM_COND(DEBUG_PRINT, "Object position : " << object_position_.x << " | " << object_position_.y);
  ROS_INFO_STREAM_COND(DEBUG_PRINT, "Target angle : " << (x_error * 180 / M_PI) << " | " << (y_error * 180 / M_PI));

  ros::Time curr_time = ros::Time::now();
  ros::Duration dur = curr_time - prev_time_;
  double delta_time = dur.nsec * 0.000000001 + dur.sec;
  prev_time_ = curr_time;

  double x_error_diff = (x_error - object_current_pan_) / delta_time;
  double y_error_diff = (y_error - object_current_tilt_) / delta_time;
  x_error_sum_ += x_error;
  y_error_sum_ += y_error;
  double x_error_target = x_error * p_gain_ + x_error_diff * d_gain_ + x_error_sum_ * i_gain_;
  double y_error_target = y_error * p_gain_ + y_error_diff * d_gain_ + y_error_sum_ * i_gain_;

  //  std_msgs::Float64MultiArray x_error_msg;
  //  x_error_msg.data.push_back(x_error);
  //  x_error_msg.data.push_back(x_error_diff);
  //  x_error_msg.data.push_back(x_error_sum_);
  //  x_error_msg.data.push_back(x_error * p_gain_);
  //  x_error_msg.data.push_back(x_error_diff * d_gain_);
  //  x_error_msg.data.push_back(x_error_sum_ * i_gain_);
  //  x_error_msg.data.push_back(x_error_target);
  //  error_pub_.publish(x_error_msg);

  ROS_INFO_STREAM_COND(DEBUG_PRINT, "------------------------  " << tracking_status << "  --------------------------------------");
  ROS_INFO_STREAM_COND(DEBUG_PRINT, "error         : " << (x_error * 180 / M_PI) << " | " << (y_error * 180 / M_PI));
  ROS_INFO_STREAM_COND(
        DEBUG_PRINT,
        "error_diff    : " << (x_error_diff * 180 / M_PI) << " | " << (y_error_diff * 180 / M_PI) << " | " << delta_time);
  ROS_INFO_STREAM_COND(
        DEBUG_PRINT,
        "error_sum    : " << (x_error_sum_ * 180 / M_PI) << " | " << (y_error_sum_ * 180 / M_PI));
  ROS_INFO_STREAM_COND(
        DEBUG_PRINT,
        "error_target  : " << (x_error_target * 180 / M_PI) << " | " << (y_error_target * 180 / M_PI) << " | P : " << p_gain_ << " | D : " << d_gain_ << " | time : " << delta_time);

  // move head joint
  publishHeadJoint(x_error_target, y_error_target);

  // args for following ball
  object_current_pan_ = x_error;
  object_current_tilt_ = y_error;
  object_current_size_ = ball_size;

  object_position_.z = 0;

  tracking_status_ = tracking_status;
  return tracking_status;
}

void ObjectTracker::publishHeadJoint(double pan, double tilt)
{
  double min_angle = 1 * M_PI / 180;
  if (fabs(pan) < min_angle && fabs(tilt) < min_angle)
    return;

  sensor_msgs::JointState head_angle_msg;

  head_angle_msg.name.push_back("head_pan");
  head_angle_msg.name.push_back("head_tilt");

  head_angle_msg.position.push_back(pan);
  head_angle_msg.position.push_back(tilt);

  head_offset_joint_pub_.publish(head_angle_msg);
}

//void ObjectTracker::scanBall()
//{
//  if (use_head_scan_ == false)
//    return;

//  // check head control module enabled
//  // ...

//  // send message to head control module
//  std_msgs::String scan_msg;
//  scan_msg.data = "scan";

//  head_scan_pub_.publish(scan_msg);
//}

void ObjectTracker::objectCallback(const darknet_ros_msgs::BoundingBoxes::ConstPtr &msg)
{
  ROS_WARN_STREAM_COND(DEBUG_PRINT, "Object Callback : " << msg->bounding_boxes.size());
  // Header : header
  // Header : image_header
  // BoundingBox[] : bounding_boxs

  // check command
  int command = getCommandFromObject(msg);

  ROS_INFO_STREAM_COND((command != NoCommand), "Command from object : " << command);

  // handle command
  switch(command)
  {
  default:
    break;

  case StartTracking:
    startTracking();
    break;

  case StopTracking:
    stopTracking();
    break;

  case SpeakObject:
    break;
  }

  //check the target
  getTargetFromMsg(msg);
}

void ObjectTracker::buttonHandlerCallback(const std_msgs::String::ConstPtr &msg)
{
  // msg->data

  if (msg->data == "mode_long")
  {
    // setLED(0x01 | 0x02 | 0x04);
  }
  else if (msg->data == "user_long")
  {
    // it's using in op3_manager
    // torque on and going to init pose
  }
  else if (msg->data == "start")
  {
    if(on_tracking_ == true)
    {
      stopTracking();
    }
    else
    {
      startTracking();
    }

  }
  else if (msg->data == "mode")
  {
    // get config
    getConfig(config_path_);

    // set module and go init pose
    readyToDemo();

    // look at init position
    lookAtInit();
  }
  else if(msg->data == "user")
  {
    // change target index
    target_index_ = (target_index_ + 1) % target_list_.size();
    object_target_ = target_list_.at(target_index_);

    ROS_INFO_STREAM("Changed the target to : " << object_target_);
  }
}

int ObjectTracker::getCommandFromObject(const darknet_ros_msgs::BoundingBoxes::ConstPtr &msg)
{
  int command = NoCommand;

  for(int ix = 0; ix < msg->bounding_boxes.size(); ix++)
  {
    if(msg->bounding_boxes[ix].Class == object_start_command_)
      command = StartTracking;
    else if(msg->bounding_boxes[ix].Class == object_stop_command_)
      command = StopTracking;
    else if(msg->bounding_boxes[ix].Class == object_speak_command_)
      command = SpeakObject;
  }

  return command;
}

void ObjectTracker::getTargetFromMsg(const darknet_ros_msgs::BoundingBoxes::ConstPtr &msg)
{
  std::vector<geometry_msgs::Point> objects;

  for(int ix = 0; ix < msg->bounding_boxes.size(); ix++)
  {
    darknet_ros_msgs::BoundingBox& bounding_box = (darknet_ros_msgs::BoundingBox&) (msg->bounding_boxes[ix]);
    // darknet_ros_msgs::BoundingBox *bounding_box = static_cast<darknet_ros_msgs::BoundingBox*>(&(msg->bounding_boxes[ix]));
    if(bounding_box.Class == object_target_)
    {
      geometry_msgs::Point recog_object;

      recog_object.x = (bounding_box.xmax + bounding_box.xmin) / 1280.0 - 1.0;
      recog_object.y = (bounding_box.ymax + bounding_box.ymin) / 720.0 - 1.0;

      //      double object_x = abs(bounding_box.xmax - bounding_box.xmin);
      //      double object_y = abs(bounding_box.ymax - bounding_box.ymin);
      //      recog_object.z = sqrt(object_x * object_x + object_y * object_y);

      recog_object.z = getDistance(bounding_box.xmax, bounding_box.ymax, bounding_box.xmin, bounding_box.ymin);

      ROS_ERROR_COND(DEBUG_PRINT, "Found Object");
      objects.push_back(recog_object);

      //return;
    }
  }

  prev_position_ = object_position_;

  switch(objects.size())
  {
  case 0:
    object_position_.z = -1;
    return;

  case 1:
    object_position_ = objects.front();
    break;

  default:
    // check the closest object with prev position
    double min_distance = -1;
    for(std::vector<geometry_msgs::Point>::iterator iter = objects.begin(); iter != objects.end(); ++iter)
    {
      double distance = getDistance(iter->x, iter->y, prev_position_.x, prev_position_.y);
      if(min_distance == -1 || min_distance > distance)
      {
        min_distance = distance;
        object_position_ = *iter;
      }
    }

    break;
  }

  // set last found time
  last_found_time_ = ros::Time::now();
}

// test
void ObjectTracker::getROSCommand(const std_msgs::String::ConstPtr &msg)
{
  std::string ros_command = "gnome-terminal -x sh -c '" + msg->data + "'";

  system(ros_command.c_str());
}

void ObjectTracker::getConfig(const std::string &config_path)
{
  if(config_path == "")
  {
    ROS_ERROR("wrong path for getting config");
    return;
  }

  YAML::Node doc;
  try
  {
    // load yaml
    doc = YAML::LoadFile(config_path.c_str());

    // object : start, stop, target
    YAML::Node object_node = doc["object"];
    object_start_command_ = object_node["start_command"].as<std::string>();
    object_stop_command_ = object_node["stop_command"].as<std::string>();
    target_list_ = object_node["target"].as< std::vector<std::string> >();
    object_target_ = target_list_.at(target_index_);
//    object_target_ = object_node["target"].as<std::string>();

    YAML::Node demo_ready_node = doc["demo_ready"];
    INIT_POSE_INDEX = demo_ready_node["motion_index"].as<int>();
    INIT_PAN = demo_ready_node["head"]["pan"].as<double>() * M_PI / 180.0;
    INIT_TILT = demo_ready_node["head"]["tilt"].as<double>() * M_PI / 180.0;

    TIME_TO_BACK = doc["time_back_to_init"].as<double>();

    // example
    // for (YAML::iterator yaml_it = tar_pose_node.begin(); yaml_it != tar_pose_node.end(); ++yaml_it)
    // joint_name = yaml_it->first.as<std::string>();
    // value = yaml_it->second.as<double>();
  } catch (const std::exception& e)
  {
    ROS_ERROR("Fail to load config file.");
    return;
  }
}

void ObjectTracker::setModule(const std::string &module_name)
{
  //  // set module to direct_control_module for this demonsration
  //  std_msgs::String module_msg;
  //  module_msg.data = module_name;

  //  set_module_pub_.publish(module_msg);

  robotis_controller_msgs::SetModule set_module_srv;
  set_module_srv.request.module_name = module_name;

  if (set_joint_module_client_.call(set_module_srv) == false)
  {
    ROS_ERROR("Failed to set module");
    return;
  }

  return ;
}

void ObjectTracker::setLED(const int led_value)
{

  robotis_controller_msgs::SyncWriteItem syncwrite_msg;
  syncwrite_msg.item_name = "LED";
  syncwrite_msg.joint_name.push_back("open-cr");
  syncwrite_msg.value.push_back(led_value);

  led_pub_.publish(syncwrite_msg);
}

void ObjectTracker::setRGBLED(int blue, int green, int red)
{
  int led_full_unit = 0x1F;
  int led_value = (blue & led_full_unit) << 10 | (green & led_full_unit) << 5 | (red & led_full_unit);
  robotis_controller_msgs::SyncWriteItem syncwrite_msg;
  syncwrite_msg.item_name = "LED_RGB";
  syncwrite_msg.joint_name.push_back("open-cr");
  syncwrite_msg.value.push_back(led_value);

  led_pub_.publish(syncwrite_msg);
}

void ObjectTracker::readyToDemo()
{
  // set to action_module
  setModule("action_module");

  // go demo pose
  playMotion(INIT_POSE_INDEX);

  usleep(1500 * 1000);

  // set to head_control_module
  setModule("head_control_module");

  is_ready_to_demo_ = true;
}

void ObjectTracker::lookAtInit()
{
  sensor_msgs::JointState head_angle_msg;

  head_angle_msg.name.push_back("head_pan");
  head_angle_msg.name.push_back("head_tilt");

  head_angle_msg.position.push_back(INIT_PAN);
  head_angle_msg.position.push_back(INIT_TILT);

  head_joint_pub_.publish(head_angle_msg);
}

void ObjectTracker::playMotion(int motion_index)
{
  std_msgs::Int32 motion_msg;
  motion_msg.data = motion_index;

  motion_index_pub_.publish(motion_msg);
}

double ObjectTracker::getDistance(double x, double y, double a, double b)
{
  double delta_x = x - a;
  double delta_y = y - b;
  return sqrt(delta_x * delta_x + delta_y * delta_y);
}

void ObjectTracker::timerThread()
{
  //set node loop rate
  ros::Rate loop_rate(25);

  //node loop
  while (ros::ok())
  {
    if(on_tracking_ == true)
    {
      // check last found flag
      ros::Duration found_dur = ros::Time::now() - last_found_time_;

      double time_from_last_found = found_dur.toSec();

      if(time_from_last_found >= TIME_TO_BACK)
      {
        // handle for waiting
        lookAtInit();
      }
    }

    //relax to fit output rate
    loop_rate.sleep();
  }
}

}

