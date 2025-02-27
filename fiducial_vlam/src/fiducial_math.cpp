
#include "fiducial_math.hpp"

#include "map.hpp"
#include "observation.hpp"
#include "transform_with_covariance.hpp"

#include "cv_bridge/cv_bridge.h"
#include "opencv2/aruco.hpp"
#include "opencv2/calib3d/calib3d.hpp"

#include <gtsam/geometry/Cal3DS2.h>
#include <gtsam/geometry/PinholeCamera.h>
#include <gtsam/geometry/Point3.h>
#include <gtsam/geometry/Pose3.h>
#include "gtsam/inference/Symbol.h"
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/nonlinear/LevenbergMarquardtOptimizer.h>
#include <gtsam/nonlinear/Marginals.h>
#include <gtsam/slam/BetweenFactor.h>
#include <gtsam/slam/PriorFactor.h>

namespace fiducial_vlam
{
// ==============================================================================
// CameraInfo::CvCameraInfo class
// ==============================================================================

  class CameraInfo::CvCameraInfo
  {
    cv::Mat camera_matrix_;
    cv::Mat dist_coeffs_;

  public:
    CvCameraInfo() = delete;

    explicit CvCameraInfo(const sensor_msgs::msg::CameraInfo &msg)
      : camera_matrix_(3, 3, CV_64F, 0.), dist_coeffs_(1, 5, CV_64F)
    {
      camera_matrix_.at<double>(0, 0) = msg.k[0];
      camera_matrix_.at<double>(0, 2) = msg.k[2];
      camera_matrix_.at<double>(1, 1) = msg.k[4];
      camera_matrix_.at<double>(1, 2) = msg.k[5];
      camera_matrix_.at<double>(2, 2) = 1.;

      // ROS and OpenCV (and everybody?) agree on this ordering: k1, k2, t1 (p1), t2 (p2), k3
      dist_coeffs_.at<double>(0) = msg.d[0];
      dist_coeffs_.at<double>(1) = msg.d[1];
      dist_coeffs_.at<double>(2) = msg.d[2];
      dist_coeffs_.at<double>(3) = msg.d[3];
      dist_coeffs_.at<double>(4) = msg.d[4];
    }

    auto &camera_matrix()
    { return camera_matrix_; }

    auto &dist_coeffs()
    { return dist_coeffs_; }
  };

// ==============================================================================
// CameraInfo class
// ==============================================================================

  CameraInfo::CameraInfo() = default;

  CameraInfo::CameraInfo(const sensor_msgs::msg::CameraInfo &camera_info_msg)
    : cv_(std::make_shared<CameraInfo::CvCameraInfo>(camera_info_msg))
  {}

// ==============================================================================
// drawDetectedMarkers function
// ==============================================================================

  static void drawDetectedMarkers(cv::InputOutputArray image,
                                  cv::InputArrayOfArrays corners,
                                  cv::InputArray ids)
  {
    // calculate colors
    auto borderColor = cv::Scalar(0, 255, 0);
    cv::Scalar textColor = borderColor;
    cv::Scalar cornerColor = borderColor;

    std::swap(textColor.val[0], textColor.val[1]);     // text color just sawp G and R
    std::swap(cornerColor.val[1], cornerColor.val[2]); // corner color just sawp G and B

    int nMarkers = static_cast<int>(corners.total());
    for (int i = 0; i < nMarkers; i++) {

      cv::Mat currentMarker = corners.getMat(i);
      CV_Assert((currentMarker.total() == 4) && (currentMarker.type() == CV_32FC2));

      // draw marker sides
      for (int j = 0; j < 4; j++) {
        cv::Point2f p0, p1;
        p0 = currentMarker.ptr<cv::Point2f>(0)[j];
        p1 = currentMarker.ptr<cv::Point2f>(0)[(j + 1) % 4];
        line(image, p0, p1, borderColor, 1);
      }

      // draw first corner mark
      rectangle(image,
                currentMarker.ptr<cv::Point2f>(0)[0] - cv::Point2f(3, 3),
                currentMarker.ptr<cv::Point2f>(0)[0] + cv::Point2f(3, 3),
                cornerColor, 1, cv::LINE_AA);

      // draw ID
//      if (ids.total() != 0) {
//        cv::Point2f cent(0, 0);
//        for (int p = 0; p < 4; p++)
//          cent += currentMarker.ptr<cv::Point2f>(0)[p];
//
//        cent = cent / 4.;
//        std::stringstream s;
//        s << "id=" << ids.getMat().ptr<int>(0)[i];
//        putText(image, s.str(), cent, cv::FONT_HERSHEY_SIMPLEX, 0.5, textColor, 2);
//      }

    }
  }

// ==============================================================================
// FiducialMath::CvFiducialMath class
// ==============================================================================

  class FiducialMath::CvFiducialMath
  {
  public:
    const CameraInfo ci_;

    explicit CvFiducialMath(const CameraInfo &camera_info)
      : ci_{camera_info}
    {}

    explicit CvFiducialMath(const sensor_msgs::msg::CameraInfo &camera_info_msg)
      : ci_(camera_info_msg)
    {}

    TransformWithCovariance solve_t_camera_marker(
      const Observation &observation,
      double marker_length)
    {
      // Build up two lists of corner points: 2D in the image frame, 3D in the marker frame
      std::vector<cv::Point3d> corners_f_marker;
      std::vector<cv::Point2f> corners_f_image;

      append_corners_f_marker(marker_length, corners_f_marker);
      append_corners_f_image(observation, corners_f_image);

      // Figure out image location.
      cv::Vec3d rvec, tvec;
      cv::solvePnP(corners_f_marker, corners_f_image,
                   ci_.cv()->camera_matrix(), ci_.cv()->dist_coeffs(),
                   rvec, tvec);

      // rvec, tvec output from solvePnp "brings points from the model coordinate system to the
      // camera coordinate system". In this case the marker frame is the model coordinate system.
      // So rvec, tvec are the transformation t_camera_marker.
      return TransformWithCovariance(to_tf2_transform(rvec, tvec));
    }

    TransformWithCovariance solve_t_map_camera(const Observations &observations,
                                               Map &map)
    {
      auto t_map_markers = map.find_t_map_markers(observations);

      // Build up two lists of corner points: 2D in the image frame, 3D in the marker frame
      std::vector<cv::Point3d> all_corners_f_map;
      std::vector<cv::Point2f> all_corners_f_image;

      for (int i = 0; i < observations.size(); i += 1) {
        auto &observation = observations.observations()[i];
        auto &t_map_marker = t_map_markers[i];
        if (t_map_marker.is_valid()) {
          append_corners_f_map(t_map_marker, map.marker_length(), all_corners_f_map);
          append_corners_f_image(observation, all_corners_f_image);
        }
      }

      // If there are no known markers in the observation set, then don't
      // try to find the camera position
      if (all_corners_f_map.empty()) {
        return TransformWithCovariance{};
      }

      // Figure out camera location.
      cv::Vec3d rvec, tvec;
      cv::solvePnP(all_corners_f_map, all_corners_f_image,
                   ci_.cv()->camera_matrix(), ci_.cv()->dist_coeffs(),
                   rvec, tvec);

      // For certain cases, there is a chance that the multi marker solvePnP will
      // return the mirror of the correct solution. So try solvePn[Ransac as well.
      if (all_corners_f_image.size() > 1 * 4 && all_corners_f_image.size() < 4 * 4) {
        cv::Vec3d rvecRansac, tvecRansac;
        cv::solvePnPRansac(all_corners_f_map, all_corners_f_image,
                           ci_.cv()->camera_matrix(), ci_.cv()->dist_coeffs(),
                           rvecRansac, tvecRansac);

        // If the pose returned from the ransac version is very different from
        // that returned from the normal version, then use the ransac results.
        // solvePnp can sometimes pick up the wrong solution (a mirror solution).
        // solvePnpRansac does a better job in that case. But solvePnp does a
        // better job smoothing out image noise so it is prefered when it works.
        if (std::abs(rvec[0] - rvecRansac[0]) > 0.5 ||
            std::abs(rvec[1] - rvecRansac[1]) > 0.5 ||
            std::abs(rvec[2] - rvecRansac[2]) > 0.5) {
          rvec = rvecRansac;
          tvec = tvecRansac;
        }
      }

      if (tvec[0] < 0) { // specific tests for bad pose determination
        int xxx = 9;
      }

      // rvec, tvec output from solvePnp "brings points from the model coordinate system to the
      // camera coordinate system". In this case the map frame is the model coordinate system.
      // So rvec, tvec are the transformation t_camera_map.
      auto tf2_t_map_camera = to_tf2_transform(rvec, tvec).inverse();
      return TransformWithCovariance(tf2_t_map_camera);
    }

    Observations detect_markers(cv_bridge::CvImagePtr &color,
                                std::shared_ptr<cv_bridge::CvImage> &color_marked)
    {
      // Todo: make the dictionary a parameter
      auto dictionary = cv::aruco::getPredefinedDictionary(cv::aruco::DICT_6X6_250);
      auto detectorParameters = cv::aruco::DetectorParameters::create();
#if (CV_VERSION_MAJOR == 4)
      // Use the new AprilTag 2 corner algorithm, much better but much slower
      detectorParameters->cornerRefinementMethod = cv::aruco::CornerRefineMethod::CORNER_REFINE_APRILTAG;
#else
      detectorParameters->doCornerRefinement = true;
#endif

      // Color to gray for detection
      cv::Mat gray;
      cv::cvtColor(color->image, gray, cv::COLOR_BGR2GRAY);

      // Detect markers
      std::vector<int> ids;
      std::vector<std::vector<cv::Point2f>> corners;
      cv::aruco::detectMarkers(gray, dictionary, corners, ids, detectorParameters);

      // Annotate the markers
      if (color_marked) {
        drawDetectedMarkers(color_marked->image, corners, ids);
      }

      // return the corners as a list of observations
      return to_observations(ids, corners);
    }

    void annotate_image_with_marker_axis(std::shared_ptr<cv_bridge::CvImage> &color_marked,
                                         const TransformWithCovariance &t_camera_marker)
    {
      cv::Vec3d rvec;
      cv::Vec3d tvec;
      to_cv_rvec_tvec(t_camera_marker, rvec, tvec);

      cv::aruco::drawAxis(color_marked->image,
                          ci_.cv()->camera_matrix(), ci_.cv()->dist_coeffs(),
                          rvec, tvec, 0.1);
    }

    void update_marker_simple_average(Marker &existing, const TransformWithCovariance &another_twc)
    {
      if (!existing.is_fixed()) {
        auto t_map_marker = existing.t_map_marker();  // Make a copy
        auto update_count = existing.update_count();
        t_map_marker.update_simple_average(another_twc, update_count);
        existing.set_t_map_marker(t_map_marker);
        existing.set_update_count(update_count + 1);
      }
    }

    void update_map(const TransformWithCovariance &t_map_camera,
                    const Observations &observations,
                    Map &map)
    {
      // For all observations estimate the marker location and update the map
      for (auto &observation : observations.observations()) {

        auto t_camera_marker = solve_t_camera_marker(observation, map.marker_length());
        auto t_map_marker = TransformWithCovariance(t_map_camera.transform() * t_camera_marker.transform());

        // Update an existing marker or add a new one.
        auto marker_ptr = map.find_marker(observation.id());
        if (marker_ptr) {
          auto &marker = *marker_ptr;
          update_marker_simple_average(marker, t_map_marker);

        } else {
          map.add_marker(Marker(observation.id(), t_map_marker));
        }
      }
    }

    void append_corners_f_map(const TransformWithCovariance &t_map_marker,
                              double marker_length,
                              std::vector<cv::Point3d> &corners_f_map)
    {
      // Build up a list of the corner locations in the marker frame.
      tf2::Vector3 corner0_f_marker(-marker_length / 2.f, marker_length / 2.f, 0.f);
      tf2::Vector3 corner1_f_marker(marker_length / 2.f, marker_length / 2.f, 0.f);
      tf2::Vector3 corner2_f_marker(marker_length / 2.f, -marker_length / 2.f, 0.f);
      tf2::Vector3 corner3_f_marker(-marker_length / 2.f, -marker_length / 2.f, 0.f);

      // Transform the corners to the map frame.
      const auto &t_map_marker_tf = t_map_marker.transform();
      auto corner0_f_map = t_map_marker_tf * corner0_f_marker;
      auto corner1_f_map = t_map_marker_tf * corner1_f_marker;
      auto corner2_f_map = t_map_marker_tf * corner2_f_marker;
      auto corner3_f_map = t_map_marker_tf * corner3_f_marker;

      corners_f_map.emplace_back(cv::Point3d(corner0_f_map.x(), corner0_f_map.y(), corner0_f_map.z()));
      corners_f_map.emplace_back(cv::Point3d(corner1_f_map.x(), corner1_f_map.y(), corner1_f_map.z()));
      corners_f_map.emplace_back(cv::Point3d(corner2_f_map.x(), corner2_f_map.y(), corner2_f_map.z()));
      corners_f_map.emplace_back(cv::Point3d(corner3_f_map.x(), corner3_f_map.y(), corner3_f_map.z()));
    }


    void append_corners_f_marker(double marker_length, std::vector<cv::Point3d> &corners_f_marker)
    {
      // Add to the list of the corner locations in the marker frame.
      corners_f_marker.emplace_back(cv::Point3d(-marker_length / 2.f, marker_length / 2.f, 0.f));
      corners_f_marker.emplace_back(cv::Point3d(marker_length / 2.f, marker_length / 2.f, 0.f));
      corners_f_marker.emplace_back(cv::Point3d(marker_length / 2.f, -marker_length / 2.f, 0.f));
      corners_f_marker.emplace_back(cv::Point3d(-marker_length / 2.f, -marker_length / 2.f, 0.f));
    }

    void append_corners_f_image(const Observation &observation, std::vector<cv::Point2f> &corners_f_image)
    {
      corners_f_image.emplace_back(
        cv::Point2f(static_cast<float>(observation.x0()), static_cast<float>(observation.y0())));
      corners_f_image.emplace_back(
        cv::Point2f(static_cast<float>(observation.x1()), static_cast<float>(observation.y1())));
      corners_f_image.emplace_back(
        cv::Point2f(static_cast<float>(observation.x2()), static_cast<float>(observation.y2())));
      corners_f_image.emplace_back(
        cv::Point2f(static_cast<float>(observation.x3()), static_cast<float>(observation.y3())));
    };

  private:
    Observations to_observations(const std::vector<int> &ids, const std::vector<std::vector<cv::Point2f>> &corners)
    {
      Observations observations;
      for (int i = 0; i < ids.size(); i += 1) {
        observations.add(Observation(ids[i],
                                     corners[i][0].x, corners[i][0].y,
                                     corners[i][1].x, corners[i][1].y,
                                     corners[i][2].x, corners[i][2].y,
                                     corners[i][3].x, corners[i][3].y));
      }
      return observations;
    }

  public:
    tf2::Transform to_tf2_transform(const cv::Vec3d &rvec, const cv::Vec3d &tvec)
    {
      tf2::Vector3 t(tvec[0], tvec[1], tvec[2]);
      cv::Mat rmat;
      cv::Rodrigues(rvec, rmat);
      tf2::Matrix3x3 m;
      for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
          m[row][col] = rmat.at<double>(row, col);  // Row- vs. column-major order
        }
      }
      tf2::Transform result(m, t);
      return result;
    }

    void to_cv_rvec_tvec(const TransformWithCovariance &t, cv::Vec3d &rvec, cv::Vec3d &tvec)
    {
      auto c = t.transform().getOrigin();
      tvec[0] = c.x();
      tvec[1] = c.y();
      tvec[2] = c.z();
      auto R = t.transform().getBasis();
      cv::Mat rmat(3, 3, CV_64FC1);
      for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
          rmat.at<double>(row, col) = R[row][col];
        }
      }
      cv::Rodrigues(rmat, rvec);
    }
  };

// ==============================================================================
// FiducialMath::SamFiducialMath class
// ==============================================================================

  class FiducialMath::SamFiducialMath
  {
    CvFiducialMath &cv_;
    gtsam::Cal3DS2 cal3ds2_;
    const gtsam::SharedNoiseModel corner_measurement_noise_;

    gtsam::Key camera_key_{gtsam::Symbol('c', 1)};


    class ResectioningFactor : public gtsam::NoiseModelFactor1<gtsam::Pose3>
    {
      const gtsam::Cal3DS2 cal3ds2_;
      const gtsam::Point3 P_;       ///< 3D point on the calibration rig
      const gtsam::Point2 p_;       ///< 2D measurement of the 3D point

    public:
      /// Construct factor given known point P and its projection p
      ResectioningFactor(const gtsam::SharedNoiseModel &model,
                         const gtsam::Key key,
                         const gtsam::Cal3DS2 &cal3ds2,
                         gtsam::Point2 p,
                         gtsam::Point3 P) :
        NoiseModelFactor1<gtsam::Pose3>(model, key),
        cal3ds2_{cal3ds2},
        P_(std::move(P)),
        p_(std::move(p))
      {}

      /// evaluate the error
      gtsam::Vector evaluateError(const gtsam::Pose3 &pose,
                                  boost::optional<gtsam::Matrix &> H) const override
      {
        auto camera = gtsam::PinholeCamera<gtsam::Cal3DS2>{pose, cal3ds2_};
        return camera.project(P_, H) - p_;
      }
    };

    gtsam::Pose3 to_pose3(const tf2::Transform &transform)
    {
      auto q = transform.getRotation();
      auto t = transform.getOrigin();
      return gtsam::Pose3{gtsam::Rot3{q.w(), q.x(), q.y(), q.z()},
                          gtsam::Vector3{t.x(), t.y(), t.z()}};
    }

    gtsam::Matrix6 to_cov_sam(const TransformWithCovariance::cov_type cov)
    {
      gtsam::Matrix6 cov_sam;
      for (int r = 0; r < 6; r += 1) {
        for (int c = 0; c < 6; c += 1) {
          static int ro[] = {3, 4, 5, 0, 1, 2};
          cov_sam(ro[r], ro[c]) = cov[r * 6 + c];
        }
      }
      return cov_sam;
    }

    TransformWithCovariance::cov_type to_cov_type(const gtsam::Pose3 &sam_pose, const gtsam::Matrix6 &cov_sam)
    {
      // Try to rotate the position part of the covariance
//      gtsam::Matrix6 rot6{};
//      rot6.setZero();
//      gtsam::Matrix3 rot3 = sam_pose.rotation().matrix();
//      for (int r = 0; r < 3; r += 1) {
//        for (int c = 0; c < 3; c += 1) {
//          rot6(r, c) = rot3(r, c);
//          rot6(r + 3, c + 3) = rot3(r, c);
//        }
//      }
//      gtsam::Matrix6 cov_sam_r = rot6 * cov_sam * rot6.transpose();

      // Convert covariance
      TransformWithCovariance::cov_type cov;
      for (int r = 0; r < 6; r += 1) {
        for (int c = 0; c < 6; c += 1) {
          static int ro[] = {3, 4, 5, 0, 1, 2};
          cov[r * 6 + c] = cov_sam(ro[r], ro[c]);
        }
      }
      return cov;
    }

    TransformWithCovariance to_transform_with_covariance(const gtsam::Pose3 &sam_pose, const gtsam::Matrix6 &sam_cov)
    {
      auto q1 = sam_pose.rotation().toQuaternion().coeffs();
      auto &t = sam_pose.translation();
      return TransformWithCovariance{
        tf2::Transform{tf2::Quaternion{q1[0], q1[1], q1[2], q1[3]},
                       tf2::Vector3{t.x(), t.y(), t.z()}},
        to_cov_type(sam_pose, sam_cov)};
    }

    TransformWithCovariance extract_transform_with_covariance(gtsam::NonlinearFactorGraph &graph,
                                                              const gtsam::Values &result,
                                                              gtsam::Key key)
    {
      gtsam::Marginals marginals(graph, result);
      return to_transform_with_covariance(result.at<gtsam::Pose3>(key),
                                          marginals.marginalCovariance(key));
    }

    TransformWithCovariance solve_camera_f_marker(
      const Observation &observation,
      double marker_length)
    {
      // 1. Allocate the graph and initial estimate
      gtsam::NonlinearFactorGraph graph{};
      gtsam::Values initial{};

      // 2. add factors to the graph
      std::vector<cv::Point3d> corners_f_marker{};
      std::vector<cv::Point2f> corners_f_image{};

      cv_.append_corners_f_marker(marker_length, corners_f_marker);
      cv_.append_corners_f_image(observation, corners_f_image);

      for (size_t j = 0; j < corners_f_image.size(); j += 1) {
        gtsam::Point2 corner_f_image{corners_f_image[j].x, corners_f_image[j].y};
        gtsam::Point3 corner_f_marker{corners_f_marker[j].x, corners_f_marker[j].y, corners_f_marker[j].z};
        graph.emplace_shared<ResectioningFactor>(corner_measurement_noise_, camera_key_,
                                                 cal3ds2_,
                                                 corner_f_image,
                                                 corner_f_marker);
      }

      // 3. Add the initial estimate for the camera pose in the marker frame
      auto cv_t_camera_marker = cv_.solve_t_camera_marker(observation, marker_length);
      auto camera_f_marker_initial = to_pose3(cv_t_camera_marker.transform().inverse());
      initial.insert(camera_key_, camera_f_marker_initial);

      // 4. Optimize the graph using Levenberg-Marquardt
      auto result = gtsam::LevenbergMarquardtOptimizer(graph, initial).optimize();

      // 5. Extract the result
      return extract_transform_with_covariance(graph, result, camera_key_);
    }

  public:
    explicit SamFiducialMath(CvFiducialMath &cv, double corner_measurement_sigma) :
      cv_{cv}, cal3ds2_{
      cv.ci_.cv()->camera_matrix().at<double>(0, 0),  // fx
      cv.ci_.cv()->camera_matrix().at<double>(1, 1),  // fy
      1.0, // s
      cv.ci_.cv()->camera_matrix().at<double>(0, 2),  // u0
      cv.ci_.cv()->camera_matrix().at<double>(1, 2),  // v0
      cv.ci_.cv()->dist_coeffs().at<double>(0), // k1
      cv.ci_.cv()->dist_coeffs().at<double>(1), // k2
      cv.ci_.cv()->dist_coeffs().at<double>(2), // p1
      cv.ci_.cv()->dist_coeffs().at<double>(3)},// p2
      corner_measurement_noise_{gtsam::noiseModel::Diagonal::Sigmas(
        gtsam::Vector2(corner_measurement_sigma, corner_measurement_sigma))}
    {}

    TransformWithCovariance solve_t_map_camera_sfm(const Observations &observations,
                                                   Map &map)
    {
      auto t_map_markers = map.find_t_map_markers(observations);

      // Get an estimate of camera_f_map.
      auto cv_t_map_camera = cv_.solve_t_map_camera(observations,
                                                    map);

      // If we could not find an estimate, then there are no known markers in the image.
      if (!cv_t_map_camera.is_valid()) {
        return cv_t_map_camera;
      }

      // 1. Allocate the graph and initial estimate
      gtsam::NonlinearFactorGraph graph{};
      gtsam::Values initial{};

      // 2. add factors to the graph
      for (int i = 0; i < observations.size(); i += 1) {
        auto &t_map_marker = t_map_markers[i];
        if (t_map_marker.is_valid()) {

          std::vector<cv::Point3d> corners_f_map{};
          std::vector<cv::Point2f> corners_f_image{};

          cv_.append_corners_f_map(t_map_markers[i], map.marker_length(), corners_f_map);
          cv_.append_corners_f_image(observations.observations()[i], corners_f_image);

          for (size_t j = 0; j < corners_f_image.size(); j += 1) {
            gtsam::Point2 corner_f_image{corners_f_image[j].x, corners_f_image[j].y};
            gtsam::Point3 corner_f_map{corners_f_map[j].x, corners_f_map[j].y, corners_f_map[j].z};
            graph.emplace_shared<ResectioningFactor>(corner_measurement_noise_, camera_key_,
                                                     cal3ds2_,
                                                     corner_f_image,
                                                     corner_f_map);
          }
        }
      }

      // 3. Add the initial estimate for the camera pose
      initial.insert(camera_key_, to_pose3(cv_t_map_camera.transform()));

      // 4. Optimize the graph using Levenberg-Marquardt
      auto result = gtsam::LevenbergMarquardtOptimizer(graph, initial).optimize();
//      std::cout << "initial error = " << graph.error(initial) << std::endl;
//      std::cout << "final error = " << graph.error(result) << std::endl;

      // 5. Extract the result
      return extract_transform_with_covariance(graph, result, camera_key_);
    }

    void load_graph_from_observations(const TransformWithCovariance &t_map_camera,
                                      const Observations &observations,
                                      Map &map,
                                      gtsam::Key camera_key, bool add_unknown_markers,
                                      gtsam::NonlinearFactorGraph &graph, gtsam::Values &initial)
    {
      // 1. clear the graph and initial estimate
      graph.resize(0);
      initial.clear();

      // 2. add measurement factors, known marker priors, and marker initial estimates to the graph
      for (auto &observation : observations.observations()) {
        gtsam::Symbol marker_key{'m', static_cast<std::uint64_t>(observation.id())};

        // See if this is a known marker by looking it up in the map.
        auto marker_ptr = map.find_marker(observation.id());

        // If this is a known marker, add the between measurement, the initial value, and add it as a prior
        if (marker_ptr != nullptr) {

          // Get the measurement
          auto camera_f_marker = solve_camera_f_marker(observation, map.marker_length());

          // Add the between factor for this measurement
          auto cov = to_cov_sam(camera_f_marker.cov());
          graph.emplace_shared<gtsam::BetweenFactor<gtsam::Pose3>>(
            marker_key,
            camera_key,
            to_pose3(camera_f_marker.transform()),
            gtsam::noiseModel::Gaussian::Covariance(cov));

          // Get the pose and covariance from the marker.
          auto known_marker_f_map = to_pose3(marker_ptr->t_map_marker().transform());
          auto known_marker_cov = to_cov_sam(marker_ptr->t_map_marker().cov());

          // Choose the noise model to use for the marker pose prior. Choose between
          // the covariance stored with the marker in the map or just a constrained model
          // that indicates that the marker pose is known precisely.
          // Use the constrained model if:
          //  the marker is fixed -> The location of the marker is known precisely
          //  or the map_style > MapStyles::pose -> there are no valid covariances
          //  or the first variance is zero -> A shortcut that says there is no variance.
          bool use_constrained = marker_ptr->is_fixed() ||
                                 map.map_style() == Map::MapStyles::pose ||
                                 known_marker_cov(0, 0) == 0.0;

          // Create the appropriate marker pose prior noise model.
          auto known_noise_model = use_constrained ?
                                   gtsam::noiseModel::Constrained::MixedSigmas(gtsam::Z_6x1) :
                                   gtsam::noiseModel::Gaussian::Covariance(known_marker_cov);

          // Add the prior for the known marker.
          graph.emplace_shared<gtsam::PriorFactor<gtsam::Pose3> >(marker_key,
                                                                  known_marker_f_map,
                                                                  known_noise_model);

          // Add the initial estimate for the known marker.
          initial.insert(marker_key,
                         known_marker_f_map);
        }

        // If this is an unknown marker, then add the measurement and just add the initial estimate.
        // Calculate the estimate from the input camera pose and the measurement.
        if (marker_ptr == nullptr && add_unknown_markers) {

          // Get the measurement
          auto camera_f_marker = solve_camera_f_marker(observation, map.marker_length());

          // Add the between factor for this measurement
          auto cov = to_cov_sam(camera_f_marker.cov());
          graph.emplace_shared<gtsam::BetweenFactor<gtsam::Pose3>>(
            marker_key,
            camera_key,
            to_pose3(camera_f_marker.transform()),
            gtsam::noiseModel::Gaussian::Covariance(cov));

          auto unknown_marker_f_map = t_map_camera.transform() * camera_f_marker.transform().inverse();
          initial.insert(marker_key,
                         to_pose3(unknown_marker_f_map));
        }
      }

      // Add the camera initial value.
      initial.insert(camera_key, to_pose3(t_map_camera.transform()));
    }

    TransformWithCovariance solve_t_map_camera(const Observations &observations,
                                               Map &map)
    {
      // Get an estimate of camera_f_map.
      auto cv_t_map_camera = cv_.solve_t_map_camera(observations,
                                                    map);

      // If we could not find an estimate, then there are no known markers in the image.
      if (!cv_t_map_camera.is_valid()) {
        return cv_t_map_camera;
      }

      // 1. Allocate the graph and initial estimate
      gtsam::NonlinearFactorGraph graph{};
      gtsam::Values initial{};

      // 2. add factors to the graph
      load_graph_from_observations(cv_t_map_camera,
                                   observations,
                                   map,
                                   camera_key_, false,
                                   graph, initial);

      // 4. Optimize the graph using Levenberg-Marquardt
      auto result = gtsam::LevenbergMarquardtOptimizer(graph, initial).optimize();
//      std::cout << "initial error = " << graph.error(initial) << std::endl;
//      std::cout << "final error = " << graph.error(result) << std::endl;

      // 5. Extract the result
      return extract_transform_with_covariance(graph, result, camera_key_);
    }

    void update_map(const TransformWithCovariance &t_map_camera,
                    const Observations &observations,
                    Map &map)
    {
      // Have to have a valid camera pose and see at least two markers before this routine can do anything.
      if (!t_map_camera.is_valid() || observations.size() < 2) {
        return;
      }

//      std::cout << "update_map known markers: " << map.markers().size() << std::endl;

      gtsam::NonlinearFactorGraph graph{};
      gtsam::Values initial{};
      gtsam::Symbol camera_key{'c', 0};

      load_graph_from_observations(t_map_camera,
                                   observations,
                                   map,
                                   camera_key, true,
                                   graph, initial);

      // Now optimize this graph
      auto result = gtsam::LevenbergMarquardtOptimizer(graph, initial).optimize();
//      std::cout << "initial error = " << graph.error(initial) << std::endl;
//      std::cout << "final error = " << graph.error(result) << std::endl;

      // Update the map
      for (auto &observation : observations.observations()) {

        gtsam::Symbol marker_key{'m', static_cast<std::uint64_t>(observation.id())};
        auto t_map_marker = extract_transform_with_covariance(graph, result, marker_key);

        // update an existing marker or add a new one.
        auto marker_ptr = map.find_marker(observation.id());
        if (marker_ptr == nullptr) {
          map.add_marker(Marker{observation.id(), t_map_marker});
        } else if (!marker_ptr->is_fixed()) {
          marker_ptr->set_t_map_marker(t_map_marker);
          marker_ptr->set_update_count(marker_ptr->update_count() + 1);
        }

        // Display the pose and cov of a marker
//        if (observation.id() == 2) {
//          auto t_map_marker_pose = to_pose3(t_map_marker.transform());
//          auto t_map_marker_cov = to_cov_sam(t_map_marker.cov());
//          std::cout << t_map_marker_pose << " : " << t_map_marker_cov << std::endl;
//        }
      }
    }
  };

// ==============================================================================
// FiducialMath class
// ==============================================================================

  FiducialMath::FiducialMath(bool sam_not_cv,
                             double corner_measurement_sigma,
                             const CameraInfo &camera_info) :
    sam_not_cv_{sam_not_cv},
    cv_{std::make_unique<CvFiducialMath>(camera_info)},
    sam_{std::make_unique<SamFiducialMath>(*cv_, corner_measurement_sigma)}
  {}

  FiducialMath::FiducialMath(bool sam_not_cv,
                             double corner_measurement_sigma,
                             const sensor_msgs::msg::CameraInfo &camera_info_msg) :
    sam_not_cv_{sam_not_cv},
    cv_{std::make_unique<CvFiducialMath>(camera_info_msg)},
    sam_{std::make_unique<SamFiducialMath>(*cv_, corner_measurement_sigma)}
  {}

  FiducialMath::~FiducialMath() = default;

  TransformWithCovariance FiducialMath::solve_t_camera_marker(
    const Observation &observation,
    double marker_length)
  {
    return cv_->solve_t_camera_marker(observation, marker_length);
  }

  TransformWithCovariance FiducialMath::solve_t_map_camera(const Observations &observations,
                                                           Map &map)
  {
    return sam_not_cv_ ?
           sam_->solve_t_map_camera(observations, map) :
           cv_->solve_t_map_camera(observations, map);
  }

  Observations FiducialMath::detect_markers(std::shared_ptr<cv_bridge::CvImage> &color,
                                            std::shared_ptr<cv_bridge::CvImage> &color_marked)
  {
    return cv_->detect_markers(color, color_marked);
  }

  void FiducialMath::annotate_image_with_marker_axis(std::shared_ptr<cv_bridge::CvImage> &color_marked,
                                                     const TransformWithCovariance &t_camera_marker)
  {
    cv_->annotate_image_with_marker_axis(color_marked, t_camera_marker);
  }

  void FiducialMath::update_map(const TransformWithCovariance &t_map_camera,
                                const Observations &observations,
                                Map &map)
  {
    if (sam_not_cv_) {
      sam_->update_map(t_map_camera, observations, map);
    } else {
      cv_->update_map(t_map_camera, observations, map);
    }
  }

}
