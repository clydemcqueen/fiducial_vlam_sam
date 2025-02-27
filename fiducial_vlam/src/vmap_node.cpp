
#include <chrono>

#include "rclcpp/rclcpp.hpp"

#include "fiducial_math.hpp"
#include "map.hpp"
#include "observation.hpp"
#include "vmap_context.hpp"

#include "tf2_geometry_msgs/tf2_geometry_msgs.h"
#include "tf2_msgs/msg/tf_message.hpp"
#include "visualization_msgs/msg/marker_array.hpp"
#include "yaml-cpp/yaml.h"

#include <iostream>
#include <iomanip>
#include <fstream>

namespace fiducial_vlam
{
// ==============================================================================
// ToYAML class
// ==============================================================================

  class ToYAML
  {
    const Map &map_;
    YAML::Emitter emitter_{};

    void do_header()
    {
      emitter_ << YAML::Key << "marker_length" << YAML::Value << map_.marker_length();
      emitter_ << YAML::Key << "map_style" << YAML::Value << map_.map_style();
    }

    void do_marker(const Marker &marker)
    {
      emitter_ << YAML::BeginMap;
      emitter_ << YAML::Key << "id" << YAML::Value << marker.id();
      emitter_ << YAML::Key << "u" << YAML::Value << marker.update_count();
      emitter_ << YAML::Key << "f" << YAML::Value << (marker.is_fixed() ? 1 : 0);

      auto &c = marker.t_map_marker().transform().getOrigin();
      emitter_ << YAML::Key << "xyz" << YAML::Value << YAML::Flow
               << YAML::BeginSeq << c.x() << c.y() << c.z() << YAML::EndSeq;

      double roll, pitch, yaw;
      marker.t_map_marker().transform().getBasis().getRPY(roll, pitch, yaw);
      emitter_ << YAML::Key << "rpy" << YAML::Value << YAML::Flow
               << YAML::BeginSeq << roll << pitch << yaw << YAML::EndSeq;

      // Save the covariance if appropriate for the map_style
      if (map_.map_style() != Map::MapStyles::pose) {
        emitter_ << YAML::Key << "cov" << YAML::Value << YAML::Flow << YAML::BeginSeq;
        for (auto cov_element : marker.t_map_marker().cov()) {
          emitter_ << cov_element;
        }
        emitter_ << YAML::EndSeq;;
      }

      emitter_ << YAML::EndMap;
    }

    void do_markers()
    {
      emitter_ << YAML::Key << "markers" << YAML::Value << YAML::BeginSeq;
      for (auto &marker_pair : map_.markers()) {
        auto &marker = marker_pair.second;
        do_marker(marker);
      }
      emitter_ << YAML::EndSeq;
    }

    void do_map()
    {
      emitter_ << YAML::BeginMap;
      do_header();
      do_markers();
      emitter_ << YAML::EndMap;
    }

  public:
    explicit ToYAML(const Map &map)
      : map_(map)
    {}

    void to_YAML(std::ostream &out_stream)
    {
      do_map();
      out_stream << emitter_.c_str() << std::endl;
    }
  };

  static std::string to_YAML_file(const std::unique_ptr<Map> &map, const std::string &filename)
  {
    std::ofstream out(filename);
    if (!out) {
      return std::string{"Config error: can not open config file for writing: "}.append(filename);
    }

    ToYAML{*map}.to_YAML(out);
    return std::string{};
  }

// ==============================================================================
// FromYAML class
// ==============================================================================

  class FromYAML
  {
    YAML::Node yaml_node_{};
    std::unique_ptr<Map> map_{};
    std::string error_msg_{};


    bool from_marker(YAML::Node &marker_node)
    {
      auto id_node = marker_node["id"];
      if (!id_node.IsScalar()) {
        return yaml_error("marker.id failed IsScalar()");
      }
      auto update_count_node = marker_node["u"];
      if (!update_count_node.IsScalar()) {
        return yaml_error("marker.update_count failed IsScalar()");
      }
      auto is_fixed_node = marker_node["f"];
      if (!is_fixed_node.IsScalar()) {
        return yaml_error("marker.is_fixed failed IsScalar()");
      }
      auto xyz_node = marker_node["xyz"];
      if (!xyz_node.IsSequence()) {
        return yaml_error("marker.xyz failed IsSequence()");
      }
      if (xyz_node.size() != 3) {
        return yaml_error("marker.xyz incorrect size");
      }
      auto rpy_node = marker_node["rpy"];
      if (!rpy_node.IsSequence()) {
        return yaml_error("marker.rpy failed IsSequence()");
      }
      if (rpy_node.size() != 3) {
        return yaml_error("marker.rpy incorrect size");
      }

      std::array<double, 3> xyz_data{};
      for (int i = 0; i < xyz_data.size(); i += 1) {
        auto i_node = xyz_node[i];
        if (!i_node.IsScalar()) {
          return yaml_error("marker.xyz[i] failed IsScalar()");
        }
        xyz_data[i] = i_node.as<double>();
      }
      std::array<double, 3> rpy_data{};
      for (int i = 0; i < rpy_data.size(); i += 1) {
        auto i_node = rpy_node[i];
        if (!i_node.IsScalar()) {
          return yaml_error("marker.rpy[i] failed IsScalar()");
        }
        rpy_data[i] = i_node.as<double>();
      }

      TransformWithCovariance::mu_type mu{
        xyz_data[0],
        xyz_data[1],
        xyz_data[2],
        rpy_data[0],
        rpy_data[1],
        rpy_data[2]};

      TransformWithCovariance::cov_type cov{{0.}};
      if (map_->map_style() != Map::MapStyles::pose) {
        auto cov_node = marker_node["cov"];
        if (!cov_node.IsSequence()) {
          return yaml_error("marker.cov failed IsSequence()");
        }
        if (cov_node.size() != 36) {
          return yaml_error("marker.cov incorrect size");
        }
        for (int i = 0; i < cov.size(); i += 1) {
          auto i_node = cov_node[i];
          if (!i_node.IsScalar()) {
            return yaml_error("marker.cov[i] failed IsScalar()");
          }
          cov[i] = i_node.as<double>();
        }
      }

      Marker marker(id_node.as<int>(), TransformWithCovariance(mu, cov));
      marker.set_is_fixed(is_fixed_node.as<int>());
      marker.set_update_count(update_count_node.as<int>());
      map_->add_marker(std::move(marker));
      return true;
    }

    bool from_markers(YAML::Node &markers_node)
    {
      for (YAML::const_iterator it = markers_node.begin(); it != markers_node.end(); ++it) {
        YAML::Node marker_node = *it;
        if (marker_node.IsMap()) {
          if (from_marker(marker_node)) {
            continue;
          }
          return false;
        }
        return yaml_error("marker failed IsMap()");
      }
      return true;
    }

    bool from_map()
    {
      if (yaml_node_.IsMap()) {
        Map::MapStyles map_style = Map::MapStyles::pose;
        auto map_style_node = yaml_node_["map_style"];
        if (map_style_node.IsScalar()) {
          map_style = static_cast<Map::MapStyles>(map_style_node.as<int>());
        }
        auto marker_length_node = yaml_node_["marker_length"];
        if (marker_length_node.IsScalar()) {
          auto marker_length = marker_length_node.as<double>();
          // create the map object now that we have the marker_length;
          map_ = std::make_unique<Map>(map_style, marker_length);
          auto markers_node = yaml_node_["markers"];
          if (markers_node.IsSequence()) {
            return from_markers(markers_node);
          }
          return yaml_error("markers failed IsSequence()");
        }
        return yaml_error("marker_length failed IsScalar()");
      }
      return yaml_error("root failed IsMap()");
    }

    bool yaml_error(const std::string &s)
    {
      error_msg_ = s;
      return false;
    }

  public:
    FromYAML() = default;

    std::string from_YAML(std::istream &in, std::unique_ptr<Map> &map)
    {
      error_msg_.clear();
      try {
        yaml_node_ = YAML::Load(in);
        if (from_map()) {
          map.swap(map_);
        }
      }
      catch (YAML::ParserException &ex) {
        error_msg_ = ex.what();
      }
      return error_msg_;
    }
  };

  static std::string from_YAML_file(const std::string &filename, std::unique_ptr<Map> &map)
  {
    std::ifstream in;
    in.open(filename, std::ifstream::in);
    if (!in.good()) {
      return std::string{"Config error: can not open config file for reading: "}.append(filename);
    }

    auto err_msg = FromYAML{}.from_YAML(in, map);
    if (!err_msg.empty()) {
      return std::string{"Config error: error parsing config file: "}
        .append(filename)
        .append(" error: ")
        .append(err_msg);
    }

    return err_msg; // no error
  }

// ==============================================================================
// VmapNode class
// ==============================================================================

  class VmapNode : public rclcpp::Node
  {
    VmapContext cxt_;
    std::unique_ptr<Map> map_{};

    int callbacks_processed_{0};

    // ROS publishers
    rclcpp::Publisher<fiducial_vlam_msgs::msg::Map>::SharedPtr fiducial_map_pub_{};
    rclcpp::Publisher<visualization_msgs::msg::MarkerArray>::SharedPtr fiducial_markers_pub_{};
    rclcpp::Publisher<tf2_msgs::msg::TFMessage>::SharedPtr tf_message_pub_{};

    rclcpp::Subscription<fiducial_vlam_msgs::msg::Observations>::SharedPtr observations_sub_{};
    rclcpp::TimerBase::SharedPtr map_pub_timer_{};


    // Special "initialize map from camera location" mode
    void initialize_map_from_observations(const Observations &observations, FiducialMath &fm)
    {
      // Find the marker with the lowest id
      int min_id = std::numeric_limits<int>::max();
      const Observation *min_obs;
      for (auto &obs : observations.observations()) {
        if (obs.id() < min_id) {
          min_id = obs.id();
          min_obs = &obs;
        }
      }

      // Find t_camera_marker
      auto t_camera_marker = fm.solve_t_camera_marker(*min_obs, cxt_.marker_length_);

      // And t_map_camera
      auto t_map_camera = cxt_.map_init_transform_;

      // Figure t_map_marker and add a marker to the map.
      auto t_map_marker = TransformWithCovariance(t_map_camera.transform() * t_camera_marker.transform());
      map_->add_marker(Marker(min_id, std::move(t_map_marker)));
    }

  public:
    VmapNode()
      : Node("vmap_node"), cxt_{*this}
    {
      // Get parameters from the command line
      cxt_.load_parameters();

      // Initialize the map. Load from file or otherwise.
      map_ = initialize_map();

//      auto s = to_YAML_string(*map_, "test");
//      auto m = from_YAML_string(s, "test");

      // ROS publishers.
      fiducial_map_pub_ = create_publisher<fiducial_vlam_msgs::msg::Map>(
        cxt_.fiducial_map_pub_topic_, 16);

      if (cxt_.publish_marker_visualizations_) {
        fiducial_markers_pub_ = create_publisher<visualization_msgs::msg::MarkerArray>(
          cxt_.fiducial_markers_pub_topic_, 16);
      }

      if (cxt_.publish_tfs_) {
        tf_message_pub_ = create_publisher<tf2_msgs::msg::TFMessage>("tf", 16);
      }

      // ROS subscriptions
      // If we are not making a map, don't bother subscribing to the observations.
      if (cxt_.make_not_use_map_) {
        observations_sub_ = create_subscription<fiducial_vlam_msgs::msg::Observations>(
          cxt_.fiducial_observations_sub_topic_,
          16,
          [this](const fiducial_vlam_msgs::msg::Observations::UniquePtr msg) -> void
          {
            this->observations_callback(msg);
          });
      }

      // Timer for publishing map info
      map_pub_timer_ = create_wall_timer(
        std::chrono::milliseconds(static_cast<int>(1000. / cxt_.marker_map_publish_frequency_hz_)),
        [this]() -> void
        {
          // Only if there is a map. There might not
          // be a map if no markers have been observed.
          if (map_) {
            this->publish_map_and_visualization();
          }
        });

      (void) observations_sub_;
      (void) map_pub_timer_;
      RCLCPP_INFO(get_logger(), "vmap_node ready");
    }

  private:

    void observations_callback(const fiducial_vlam_msgs::msg::Observations::UniquePtr &msg)
    {
      callbacks_processed_ += 1;

      CameraInfo ci{msg->camera_info};
      FiducialMath fm{cxt_.sam_not_cv_, cxt_.corner_measurement_sigma_, ci};

      // Get observations from the message.
      Observations observations(*msg);

      // If the map has not yet been initialized, then initialize it with these observations.
      // This is only used for the special camera based map initialization
      if (!map_ && observations.size() > 0) {
        initialize_map_from_observations(observations, fm);
      }

      // There is nothing to do at this point unless we have more than one observation.
      if (observations.size() < 2) {
        return;
      }

      // Estimate the camera pose using the latest map estimate
      auto t_map_camera = fm.solve_t_map_camera(observations, *map_);

      // We get an invalid pose if none of the visible markers pose's are known.
      if (t_map_camera.is_valid()) {

        // Update our map with the observations
        fm.update_map(t_map_camera, observations, *map_);
      }
    }

    tf2_msgs::msg::TFMessage to_tf_message()
    {
      auto stamp = now();
      tf2_msgs::msg::TFMessage tf_message;

      for (auto &marker_pair: map_->markers()) {
        auto &marker = marker_pair.second;
        auto mu = marker.t_map_marker().mu();

        std::ostringstream oss_child_frame_id;
        oss_child_frame_id << cxt_.marker_prefix_frame_id_ << std::setfill('0') << std::setw(3) << marker.id();

        tf2::Quaternion q;
        q.setRPY(mu[3], mu[4], mu[5]);
        auto tf2_transform = tf2::Transform(q, tf2::Vector3(mu[0], mu[1], mu[2]));

        geometry_msgs::msg::TransformStamped msg;
        msg.header.stamp = stamp;
        msg.header.frame_id = cxt_.map_frame_id_;
        msg.child_frame_id = oss_child_frame_id.str();
        msg.transform = tf2::toMsg(tf2_transform);

        tf_message.transforms.emplace_back(msg);
      }

      return tf_message;
    }

    visualization_msgs::msg::MarkerArray to_marker_array_msg()
    {
      visualization_msgs::msg::MarkerArray markers;
      for (auto &marker_pair: map_->markers()) {
        auto &marker = marker_pair.second;
        visualization_msgs::msg::Marker marker_msg;
        marker_msg.id = marker.id();
        marker_msg.header.frame_id = cxt_.map_frame_id_;
        marker_msg.pose = to_Pose_msg(marker.t_map_marker());
        marker_msg.type = visualization_msgs::msg::Marker::CUBE;
        marker_msg.action = visualization_msgs::msg::Marker::ADD;
        marker_msg.scale.x = 0.1;
        marker_msg.scale.y = 0.1;
        marker_msg.scale.z = 0.01;
        marker_msg.color.r = 1.f;
        marker_msg.color.g = 1.f;
        marker_msg.color.b = 0.f;
        marker_msg.color.a = 1.f;
        markers.markers.emplace_back(marker_msg);
      }
      return markers;
    }

    void publish_map_and_visualization()
    {
      // publish the map
      std_msgs::msg::Header header;
      header.stamp = now();
      header.frame_id = cxt_.map_frame_id_;
      fiducial_map_pub_->publish(*map_->to_map_msg(header));

      // Publish the marker Visualization
      if (cxt_.publish_marker_visualizations_) {
        fiducial_markers_pub_->publish(to_marker_array_msg());
      }

      // Publish the transform tree
      if (cxt_.publish_tfs_) {
        tf_message_pub_->publish(to_tf_message());
      }

      // Save the map
      if (cxt_.make_not_use_map_ && !cxt_.marker_map_save_full_filename_.empty()) {
        auto err_msg = to_YAML_file(map_, cxt_.marker_map_save_full_filename_);
        if (!err_msg.empty()) {
          RCLCPP_INFO(get_logger(), err_msg.c_str());
        }
      }
    }

    std::unique_ptr<Map> initialize_map()
    {
      std::unique_ptr<Map> map_unique{};

      // If not building a map, then load the map from a file
      if (!cxt_.make_not_use_map_) {
        RCLCPP_INFO(get_logger(), "Loading map file '%s'", cxt_.marker_map_load_full_filename_.c_str());

        // load the map.
        auto err_msg = from_YAML_file(cxt_.marker_map_load_full_filename_, map_unique);

        if (err_msg.empty()) {
          return map_unique;
        }
        // If an error, fall into initialize the map
        RCLCPP_ERROR(get_logger(), err_msg.c_str());
        RCLCPP_ERROR(get_logger(), "Falling into initialize map. (style: %d)", cxt_.map_init_style_);
      }

      // Building a map. Use the different styles of map initialization.
      // If style 2, then need to wait for an observation for initialization.
      if (cxt_.map_init_style_ == 2) {
        return map_unique;
      }

      // Base the style of the new map on the sam_not_cv parameter. If we are not
      // doing sam, then the map contains only poses.
      Map::MapStyles new_map_style = cxt_.sam_not_cv_ ?
                                     Map::MapStyles::covariance :
                                     Map::MapStyles::pose;

      // if Style == 0, look for a file and pull the pose from it.
      // If there is a problem, fall into style 1.
      if (cxt_.map_init_style_ == 0) {
        std::unique_ptr<Map> map_temp{};
        auto err_msg = from_YAML_file(cxt_.marker_map_load_full_filename_, map_temp);
        if (!err_msg.empty()) {
          RCLCPP_ERROR(get_logger(), "Error while trying to initialize map style 0");
          RCLCPP_ERROR(get_logger(), err_msg.c_str());
          RCLCPP_ERROR(get_logger(), "Falling into initialize map style 1");

        } else {
          auto marker_temp = map_temp->find_marker(cxt_.map_init_id_);
          if (marker_temp == nullptr) {
            RCLCPP_ERROR(get_logger(), "Error while trying to initialize map style 0");
            RCLCPP_ERROR(get_logger(), "Map file '%s' does not contain a marker with id %d",
                         cxt_.marker_map_load_full_filename_.c_str(), cxt_.map_init_id_);
            RCLCPP_ERROR(get_logger(), "Falling into initialize map style 1");

          } else {
            auto marker_copy = *marker_temp;
            marker_copy.set_is_fixed(true);
            map_unique = std::make_unique<Map>(new_map_style, cxt_.marker_length_);
            map_unique->add_marker(std::move(marker_copy));
            return map_unique;
          }
        }
      }

      // Style 1 initialization. Get the info from parameters.
      map_unique = std::make_unique<Map>(new_map_style, cxt_.marker_length_);
      auto marker_new = Marker(cxt_.map_init_id_, cxt_.map_init_transform_);
      marker_new.set_is_fixed(true);
      map_unique->add_marker(std::move(marker_new));

      return map_unique;
    }
  };
}

// ==============================================================================
// main()
// ==============================================================================

int main(int argc, char **argv)
{
  // Force flush of the stdout buffer
  setvbuf(stdout, nullptr, _IONBF, BUFSIZ);

  // Init ROS
  rclcpp::init(argc, argv);

  // Create node
  auto node = std::make_shared<fiducial_vlam::VmapNode>();
  auto result = rcutils_logging_set_logger_level(node->get_logger().get_name(), RCUTILS_LOG_SEVERITY_INFO);
  (void) result;

  // Spin until rclcpp::ok() returns false
  rclcpp::spin(node);

  // Shut down ROS
  rclcpp::shutdown();

  return 0;
}
