#include "sign_detector/lsd.hpp"
#include "sign_detector/util.hpp"

void LineSegmentDetector::imageCallback(const sensor_msgs::msg::CompressedImage& msg) const
{
  sensor_msgs::msg::Image::ConstSharedPtr image_ptr = decompressImage(msg);
  cv::Mat image = cv_bridge::toCvCopy(*image_ptr, "rgb8")->image;
  cv::Size size = image.size();

  if (!info_.has_value()) return;
  cv::Mat K = cv::Mat(cv::Size(3, 3), CV_64FC1, (void*)(info_->k.data()));
  cv::Mat D = cv::Mat(cv::Size(5, 1), CV_64FC1, (void*)(info_->d.data()));
  cv::Mat undistorted;
  cv::undistort(image, undistorted, K, D, K);
  image = undistorted;


  const int WIDTH = 800;
  const float SCALE = 1.0f * WIDTH / size.width;
  int HEIGHT = SCALE * size.height;
  cv::resize(image, image, cv::Size(WIDTH, HEIGHT));
  cv::cvtColor(image, image, cv::COLOR_BGR2GRAY);

  std::chrono::time_point start = std::chrono::system_clock::now();
  cv::Mat lines;
  lsd->detect(image, lines);
  lsd->drawSegments(image, lines);

  auto dur = std::chrono::system_clock::now() - start;
  long ms = std::chrono::duration_cast<std::chrono::milliseconds>(dur).count();
  RCLCPP_INFO_STREAM(this->get_logger(), cv::Size(WIDTH, HEIGHT) << " " << ms);

  {
    cv_bridge::CvImage raw_image;
    raw_image.header.stamp = msg.header.stamp;
    raw_image.header.frame_id = "map";
    raw_image.encoding = "bgr8";
    raw_image.image = image;
    pub_image_lsd_->publish(*raw_image.toImageMsg());
  }

  cv::Mat scaled_K = SCALE * K;
  scaled_K.at<double>(2, 2) = 1;
  projectEdgeOnPlane(lines, scaled_K, msg.header.stamp);
}

void LineSegmentDetector::listenExtrinsicTf(const std::string& frame_id)
{
  try {
    geometry_msgs::msg::TransformStamped ts = tf_buffer_->lookupTransform("base_link", frame_id, tf2::TimePointZero);
    Eigen::Vector3f p;
    p.x() = ts.transform.translation.x;
    p.y() = ts.transform.translation.y;
    p.z() = ts.transform.translation.z;

    Eigen::Quaternionf q;
    q.w() = ts.transform.rotation.w;
    q.x() = ts.transform.rotation.x;
    q.y() = ts.transform.rotation.y;
    q.z() = ts.transform.rotation.z;
    camera_extrinsic_ = Eigen::Affine3f::Identity();
    camera_extrinsic_->translation() = p;
    camera_extrinsic_->matrix().topLeftCorner(3, 3) = q.toRotationMatrix();
  } catch (tf2::TransformException& ex) {
  }
}

void LineSegmentDetector::projectEdgeOnPlane(const cv::Mat& lines, const cv::Mat& K_cv, const rclcpp::Time& stamp) const
{
  if (!camera_extrinsic_.has_value()) {
    RCLCPP_WARN_STREAM(this->get_logger(), "camera_extrinsic_ has not been initialized");
    return;
  }
  Eigen::Vector3f t = camera_extrinsic_->translation();
  Eigen::Quaternionf q(camera_extrinsic_->rotation());
  RCLCPP_INFO_STREAM(this->get_logger(), "transform: " << t.transpose() << " " << q.coeffs().transpose());

  struct Edge {
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW
    Edge(const Eigen::Vector3f& p, const Eigen::Vector3f& q) : p(p), q(q) {}
    Eigen::Vector3f p, q;
  };

  // Convert to projected coordinate
  Eigen::Matrix3f K, K_inv;
  cv::cv2eigen(K_cv, K);
  K_inv = K.inverse();

  // NOTE: reference capture is better?
  auto conv = [t, q, K_inv](const Eigen::Vector2f& u) -> Eigen::Vector3f {
    Eigen::Vector3f u3(u.x(), u.y(), 1);
    Eigen::Vector3f bearing = (q * K_inv * u3).normalized();
    if (bearing.z() > -0.1) return Eigen::Vector3f::Zero();

    float l = -t.z() / bearing.z();
    float projected_x = t.x() + bearing.x() * l;
    float projected_y = t.y() + bearing.y() * l;
    Eigen::Vector3f tmp(projected_x, projected_y, 0);
    return tmp;
  };

  std::vector<Edge> edges;
  const int N = lines.rows;
  for (int i = 0; i < N; i++) {
    cv::Mat xyxy = lines.row(i);
    Eigen::Vector2f xy1, xy2;
    xy1 << xyxy.at<float>(0), xyxy.at<float>(1);
    xy2 << xyxy.at<float>(2), xyxy.at<float>(3);
    edges.emplace_back(conv(xy1), conv(xy2));
  }

  // Draw projected edge image
  cv::Mat image = cv::Mat::zeros(cv::Size{image_size_, image_size_}, CV_8UC3);
  {
    const cv::Size center(image.cols / 2, image.rows / 2);
    auto toCvPoint = [center, this](const Eigen::Vector3f& v) -> cv::Point {
      cv::Point pt;
      pt.x = -v.y() / this->max_range_ * center.width + center.width;
      pt.y = -v.x() / this->max_range_ * center.height + 2 * center.height;
      return pt;
    };
    for (const auto e : edges) {
      if (e.p.isZero() || e.q.isZero()) continue;
      cv::line(image, toCvPoint(e.p), toCvPoint(e.q), cv::Scalar(0, 255, 255), 2, cv::LineTypes::LINE_8);
    }
  }

  // Publish image
  {
    cv_bridge::CvImage raw_image;
    raw_image.header.stamp = stamp;
    raw_image.header.frame_id = "map";
    raw_image.encoding = "bgr8";
    raw_image.image = image;
    pub_image_->publish(*raw_image.toImageMsg());
  }
}