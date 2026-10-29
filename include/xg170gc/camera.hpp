#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace xg170gc {

struct Options {
  std::string serial;                 // Empty: first enumerated camera.
  int width=0, height=0, offset_x=0, offset_y=0; // 0/0: native full frame.
  double exposure_us=1000;
  int analog_gain=144, frame_speed=0;
  int sdk_buffers=64, packet_size=0;   // 0: SDK packet-size negotiation.
  int socket_buffer_bytes=64*1024*1024;
  int queue_capacity=32, frame_pool_size=64, timeout_ms=1000;
};

struct Metadata {
  std::uint64_t capture_index=0, host_unix_ns=0, host_monotonic_ns=0;
  std::uint64_t device_timestamp_us=0;
  std::uint32_t device_frame_id=0, sdk_timestamp_0p1ms=0, exposure_us=0;
  bool device_frame_id_valid=false, device_timestamp_valid=false;
};

struct Frame {
  int width=0, height=0;
  std::uint32_t media_type=0;
  std::string encoding;
  Metadata metadata;
  std::vector<std::uint8_t> pixels;     // Owned copy, never vendor-owned memory.
};
using FramePtr=std::shared_ptr<const Frame>;

struct DeviceInfo {
  std::string model, name, serial, sdk_version, encoding;
  int width=0, height=0, offset_x=0, offset_y=0;
  double exposure_us=0;
  int analog_gain=0;
  std::uint64_t host_rmem_max=0;
  unsigned data_sockets_found=0;
  int minimum_effective_socket_buffer=0;
};

struct Statistics {
  std::uint64_t captured=0, enqueued=0, delivered=0;
  std::uint64_t queue_overruns=0, pool_exhaustions=0, get_buffer_errors=0;
  std::uint64_t frame_id_gaps=0, frame_id_wraps=0, frame_id_resets=0, frame_id_duplicates=0;
  std::uint64_t valid_frame_ids=0, valid_device_timestamps=0;
  std::uint64_t host_udp_rcvbuf_errors=0; // Host-wide, not camera-local.
  bool host_udp_counter_valid=false, sdk_statistics_valid=false, sdk_resends_valid=false;
  std::uint32_t sdk_capture_count=0, sdk_lost_count=0, sdk_resend_count=0;
  std::string error;
};

// One active Camera per process: vendor system options/socket selection are
// process-wide. Do not open another SDK/ROS camera handle beside this object.
// Constructor configures and allocates; it does not start exposure.
class Camera {
 public:
  explicit Camera(const Options& options=Options{});
  ~Camera();
  Camera(const Camera&)=delete;
  Camera& operator=(const Camera&)=delete;

  void start();                       // Start only after consumers are ready.
  bool read(FramePtr& frame, int timeout_ms=1000);
  void stop();                        // Join capture, preserve queued frames.
  DeviceInfo info() const;
  Statistics statistics() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace xg170gc
