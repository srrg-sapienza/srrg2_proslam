#include "raw_data_preprocessor_monocular_depth.h"

namespace srrg2_proslam {
  using namespace srrg2_core;

  bool RawDataPreprocessorMonocularDepth::setRawData(BaseSensorMessagePtr msg_) {
    PROFILE_TIME("RawDataPreprocessorMonocularDepth::setMeasurement");
    BaseType::setRawData(msg_);
    _intensity_message = nullptr;
    _depth_message     = nullptr;
    _status            = Error;

    // ds measurement must be a pack of exactly two images - fails also for nullptr
    MessagePackPtr message_pack = std::dynamic_pointer_cast<MessagePack>(msg_);
    if (!message_pack) {
      std::cerr << "RawDataPreprocessorMonocularDepth::setMeasurement|received "
                   "invalid message (not a message pack)"
                << std::endl;
      return false;
    }
    if (message_pack->messages.size() < 2) {
      std::cerr << "RawDataPreprocessorMonocularDepth::setMeasurement|received "
                   "invalid message pack (size: " +
                     std::to_string(message_pack->messages.size()) + " != 2)"
                << std::endl;
      return false;
    }

    // ds check left and right image message types TODO softcode order?
    _intensity_message = srrg2_slam_interfaces::extractMessage<srrg2_core::ImageMessage>(
      msg_, param_topic_rgb.value());
    if (!_intensity_message) {
      throw std::runtime_error("RawDataPreprocessorMonocularDepth::setMeasurement|ERROR, "
                               "ImageMessage not found - topic [ " +
                               param_topic_rgb.value() + " ]");
    }

    _depth_message = srrg2_slam_interfaces::extractMessage<srrg2_core::ImageMessage>(
      msg_, param_topic_depth.value());
    if (!_depth_message) {
      throw std::runtime_error("RawDataPreprocessorMonocularDepth::setMeasurement|ERROR, "
                               "ImageMessage not found - topic [ " +
                               param_topic_depth.value() + " ]");
    }

    _status = Initializing;
    return true;
  }

  void RawDataPreprocessorMonocularDepth::compute() {
    PROFILE_TIME("RawDataPreprocessorMonocularDepth::compute");
    _status = Error;
    if (!_raw_data || !_intensity_message || !_depth_message) {
      throw std::runtime_error(
        "RawDataPreprocessorMonocularDepth::compute|ERROR: measurement not set");
      return;
    }
    if (!_meas) {
      throw std::runtime_error(
        "RawDataPreprocessorMonocularDepth::compute|ERROR: destination buffer not set");
    }

    // ds if measurement hasn't been changed ignore the invocation
    if (!_raw_data_changed_flag) {
      _status = Ready;
      return;
    }
    _meas->clear();

    // ds load image intensity data from disk
    const BaseImage* image_intensity = _intensity_message->image();
    if (!image_intensity) {
      throw std::runtime_error(
        "RawDataPreprocessorMonocularDepth::compute|ERROR: unable to load intensity "
        "image from disk");
      return;
    }
    if (image_intensity->rows() == 0) {
      throw std::runtime_error(
        "RawDataPreprocessorMonocularDepth::compute|ERROR: intensity image has zero rows");
      return;
    }
    if (image_intensity->cols() == 0) {
      throw std::runtime_error(
        "RawDataPreprocessorMonocularDepth::compute|ERROR: intensity image has zero columns");
      return;
    }

    // ds detect features
    param_feature_extractor->setFeatures(_meas);
    param_feature_extractor->setProjections(_projections, _projection_radius);
    param_feature_extractor->compute(image_intensity);
    assert(image_intensity->rows() == param_feature_extractor->imageRows());
    assert(image_intensity->cols() == param_feature_extractor->imageCols());
    if (_meas->empty()) {
      std::cerr << "RawDataPreprocessorMonocularDepth::compute|WARNING: no features found"
                << std::endl;
    }
    const size_t number_of_points_in_image = _meas->size();

    // ds load depth data from disk depending on type
    BaseImage* image_depth_raw = _depth_message->image();
    if (!image_depth_raw) {
      throw std::runtime_error(
        "RawDataPreprocessorMonocularDepth::compute|ERROR: unable to load intensity "
        "image from disk");
      return;
    }
    if (image_depth_raw->rows() == 0) {
      throw std::runtime_error(
        "RawDataPreprocessorMonocularDepth::compute|ERROR: depth image has zero rows");
      return;
    }
    if (image_depth_raw->cols() == 0) {
      throw std::runtime_error(
        "RawDataPreprocessorMonocularDepth::compute|ERROR: depth image has zero columns");
      return;
    }

    // ds TODO refactor this 99% boiler switch
    switch (image_depth_raw->type()) {
      case TYPE_16UC1: {
        _readDepth<ImageUInt16>(image_depth_raw, param_depth_scaling_factor_to_meters.value());
        break;
      }
      case TYPE_32FC1: {
        _readDepth<ImageFloat>(image_depth_raw, param_depth_scaling_factor_to_meters.value());
        break;
      }
      default: {
        throw std::runtime_error(
          "RawDataPreprocessorMonocularDepth::compute|ERROR: unknown depth image type");
      }
    }

    // ds if no points are available
    if (_meas->empty()) {
      std::cerr << "RawDataPreprocessorMonocularDepth::compute|"
                << FG_YELLOW("WARNING: no adapted measurements generated") << std::endl;
      _status = Error;
      return;
    }

    // ds if we have a very high invalid depth per point ratio
    const size_t number_of_points_without_depth = number_of_points_in_image - _meas->size();
    if (static_cast<float>(number_of_points_without_depth) / number_of_points_in_image > 0.25) {
      std::cerr << "RawDataPreprocessorMonocularDepth::compute|WARNING: high number of points "
                   "without depth: "
                << number_of_points_without_depth << "/" << number_of_points_in_image << std::endl;
    }
    //    std::cerr << "RawDataPreprocessorMonocularDepth::compute|points: " << _dest->size() << "/"
    //              << number_of_points_in_image << std::endl;
    _status = Ready;
  }

  template <typename DepthImageType_>
  void RawDataPreprocessorMonocularDepth::_readDepth(srrg2_core::BaseImage* image_depth_raw_,
                                                     const float& depth_scaling_factor_) {
    const DepthImageType_* image_depth = dynamic_cast<DepthImageType_*>(image_depth_raw_);
    if (!image_depth) {
      throw std::runtime_error(
        "RawDataPreprocessorMonocularDepth::_readDepth|ERROR: unable to cast depth image");
    }

    // ds populate result buffer with depth times the scaling
    size_t index_with_depth = 0;
    for (size_t index = 0; index < _meas->size(); ++index) {
      srrg2_core::PointIntensityDescriptor3f& feature((*_meas)[index]);
      const float depth_unscaled =
        image_depth->at(std::rint(feature.coordinates()(1)), std::rint(feature.coordinates()(0)));

      // ds if depth is valid - keep the point
      if (depth_unscaled > 0) {
        feature.coordinates()(2)   = depth_scaling_factor_ * depth_unscaled;
        (*_meas)[index_with_depth] = feature;
        ++index_with_depth;
      }
    }
    _meas->resize(index_with_depth);
  }

} // namespace srrg2_slam_interfaces
