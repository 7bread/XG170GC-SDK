#include "xg170gc/camera.hpp"
#include "CameraApi.h"
#include "transport_diagnostics.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace xg170gc {
namespace {
std::atomic<bool> camera_in_use{false};
std::once_flag sdk_initialized;
void check(int status, const char* operation) {
  if (status!=CAMERA_STATUS_SUCCESS)
    throw std::runtime_error(std::string(operation)+" failed: "+std::to_string(status));
}
std::string encoding(UINT type) {
  if (type==CAMERA_MEDIA_TYPE_MONO8) return "mono8";
  if (type==CAMERA_MEDIA_TYPE_BAYRG8) return "bayer_rggb8";
  if (type==CAMERA_MEDIA_TYPE_BAYGR8) return "bayer_grbg8";
  if (type==CAMERA_MEDIA_TYPE_BAYGB8) return "bayer_gbrg8";
  if (type==CAMERA_MEDIA_TYPE_BAYBG8) return "bayer_bggr8";
  throw std::runtime_error("Unsupported raw format; an 8-bit Mono/Bayer format is required");
}
struct BufferGuard {
  int camera;
  BYTE* data;
  ~BufferGuard() {if (data) CameraReleaseImageBuffer(camera,data);}
  void release() {
    BYTE* pointer=data;data=nullptr;
    check(CameraReleaseImageBuffer(camera,pointer),"CameraReleaseImageBuffer");
  }
};
} // namespace

struct Camera::Impl {
  explicit Impl(const Options& value):options(value) {
    if (camera_in_use.exchange(true))
      throw std::runtime_error("Only one active Camera is supported per process");
    try {configure();}
    catch (...) {close();throw;}
  }
  ~Impl() {stop();close();}

  void configure() {
    if (options.width<0 || options.height<0 || (!options.width!=!options.height) ||
        options.offset_x<0 || options.offset_y<0 ||
        !std::isfinite(options.exposure_us) || options.exposure_us<=0 ||
        !camera_transport::validOptions(options.sdk_buffers,options.packet_size) ||
        options.sdk_buffers<1 || options.socket_buffer_bytes<0 || options.socket_buffer_bytes>128*1024*1024 ||
        options.queue_capacity<1 || options.queue_capacity>128 ||
        options.frame_pool_size<options.queue_capacity+2 || options.frame_pool_size>256 ||
        options.timeout_ms<1 || options.timeout_ms>5000)
      throw std::invalid_argument("Invalid camera geometry, exposure or bounded buffer options");
    std::call_once(sdk_initialized,[] {check(CameraSdkInit(0),"CameraSdkInit");});
    char version[256]{};
    check(CameraSdkGetVersionString(version),"CameraSdkGetVersionString");device.sdk_version=version;
    check(CameraSetSysOption("NumBuffers",std::to_string(options.sdk_buffers).c_str()),"NumBuffers");
    if (options.packet_size)
      check(CameraSetSysOption("PacketSize",std::to_string(options.packet_size).c_str()),"PacketSize");
    tSdkCameraDevInfo devices[16]{};int count=16;
    check(CameraEnumerateDevice(devices,&count),"CameraEnumerateDevice");
    if (count<1 || count>16) throw std::runtime_error("No camera found or invalid enumeration count");
    int selected=-1;
    for (int i=0;i<count;++i)
      if (options.serial.empty() || options.serial==devices[i].acSn) {selected=i;break;}
    if (selected<0) throw std::runtime_error("Requested camera serial was not enumerated");
    device.model=devices[selected].acProductName;
    device.name=devices[selected].acFriendlyName;device.serial=devices[selected].acSn;
    check(CameraInit(&devices[selected],-1,-1,&handle),"CameraInit");
    tSdkCameraCapbility cap{};
    check(CameraGetCapability(handle,&cap),"CameraGetCapability");
    const int max_width=cap.sResolutionRange.iWidthMax,max_height=cap.sResolutionRange.iHeightMax;
    if (max_width<=0 || max_height<=0) throw std::runtime_error("Invalid native sensor geometry");
    const bool full=!options.width;
    device.width=full?max_width:options.width;device.height=full?max_height:options.height;
    device.offset_x=full?0:options.offset_x;device.offset_y=full?0:options.offset_y;
    if (device.width>max_width || device.height>max_height ||
        device.offset_x>max_width-device.width || device.offset_y>max_height-device.height)
      throw std::invalid_argument("Requested ROI exceeds sensor bounds");
    tSdkImageResolution resolution{};resolution.iIndex=0xff;
    resolution.iWidth=resolution.iWidthFOV=device.width;
    resolution.iHeight=resolution.iHeightFOV=device.height;
    resolution.iHOffsetFOV=device.offset_x;resolution.iVOffsetFOV=device.offset_y;
    check(CameraSetImageResolution(handle,&resolution),"CameraSetImageResolution");
    tSdkImageResolution actual{};
    check(CameraGetImageResolution(handle,&actual),"CameraGetImageResolution");
    if (actual.iWidth!=device.width || actual.iHeight!=device.height ||
        actual.iWidthFOV!=device.width || actual.iHeightFOV!=device.height ||
        actual.iHOffsetFOV!=device.offset_x || actual.iVOffsetFOV!=device.offset_y ||
        actual.uBinSumMode || actual.uBinAverageMode || actual.uSkipMode || actual.uResampleMask ||
        actual.iWidthZoomHd || actual.iHeightZoomHd || actual.iWidthZoomSw || actual.iHeightZoomSw)
      throw std::runtime_error("ROI readback mismatch or scaling/binning unexpectedly enabled");
    if (!cap.pMediaTypeDesc || cap.iMediaTypdeDesc<=0)
      throw std::runtime_error("SDK did not enumerate sensor formats");
    int mono=-1,bayer=-1;
    for (int i=0;i<cap.iMediaTypdeDesc;++i) {
      UINT type=cap.pMediaTypeDesc[i].iMediaType;
      if (type==CAMERA_MEDIA_TYPE_MONO8) mono=i;
      if (type==CAMERA_MEDIA_TYPE_BAYRG8 || type==CAMERA_MEDIA_TYPE_BAYGR8 ||
          type==CAMERA_MEDIA_TYPE_BAYGB8 || type==CAMERA_MEDIA_TYPE_BAYBG8) bayer=i;
    }
    int selected_format=cap.sIspCapacity.bMonoSensor?mono:(bayer>=0?bayer:mono);
    if (selected_format<0) throw std::runtime_error("8-bit sensor transport format not available");
    check(CameraSetMediaType(handle,selected_format),"CameraSetMediaType");
    int actual_format=-1;check(CameraGetMediaType(handle,&actual_format),"CameraGetMediaType");
    if (actual_format<0 || actual_format>=cap.iMediaTypdeDesc)
      throw std::runtime_error("Invalid sensor format readback");
    raw_type=cap.pMediaTypeDesc[actual_format].iMediaType;
    device.encoding=encoding(raw_type);
    check(CameraSetTriggerMode(handle,0),"CameraSetTriggerMode");
    check(CameraSetAeState(handle,FALSE),"CameraSetAeState");
    check(CameraSetExposureTime(handle,options.exposure_us),"CameraSetExposureTime");
    check(CameraSetAnalogGain(handle,options.analog_gain),"CameraSetAnalogGain");
    check(CameraSetFrameSpeed(handle,options.frame_speed),"CameraSetFrameSpeed");
    check(CameraSetMirror(handle,0,FALSE),"horizontal mirror");
    check(CameraSetMirror(handle,1,FALSE),"vertical mirror");
    check(CameraGetExposureTime(handle,&device.exposure_us),"exposure readback");
    check(CameraGetAnalogGain(handle,&device.analog_gain),"gain readback");
    if (!std::isfinite(device.exposure_us) ||
        std::abs(device.exposure_us-options.exposure_us)>std::max(2.,options.exposure_us*.01) ||
        device.analog_gain!=options.analog_gain)
      throw std::runtime_error("Exposure/gain readback differs from request");
    device.host_rmem_max=camera_transport::receiveBufferLimit();
    const auto sockets=camera_transport::enlargeDataSockets(options.socket_buffer_bytes);
    device.data_sockets_found=sockets.size();
    device.minimum_effective_socket_buffer=sockets.empty()?0:std::numeric_limits<int>::max();
    for (const auto& socket:sockets) {
      device.minimum_effective_socket_buffer=std::min(device.minimum_effective_socket_buffer,socket.after);
      if (socket.error || socket.after<2LL*options.socket_buffer_bytes)
        throw std::runtime_error("Camera UDP receive buffer is limited; configure host net.core.rmem_max first");
    }
    if (options.socket_buffer_bytes && sockets.empty())
      throw std::runtime_error("Cannot locate SDK UDP/3959 data socket; tuning was not verified");
    for (int i=0;i<options.frame_pool_size;++i) {
      auto frame=std::make_shared<Frame>();
      frame->width=device.width;frame->height=device.height;
      frame->encoding=device.encoding;frame->media_type=raw_type;
      frame->pixels.resize(std::size_t(device.width)*device.height);
      pool.push_back(std::move(frame));
    }
  }

  void close() noexcept {
    if (handle>=0) {CameraUnInit(handle);handle=-1;}
    camera_in_use=false;
  }
  void start() {
    std::unique_lock<std::mutex> lock(mutex);
    if (ever_started) throw std::logic_error("Camera can only be started once; construct a new instance to restart");
    ever_started=true;
    worker=std::thread([this] {capture();});
    ready.wait(lock,[this] {return startup_complete;});
    if (!stats.error.empty()) {
      std::string error=stats.error;lock.unlock();stop();throw std::runtime_error(error);
    }
  }
  void stop() noexcept {
    stopping=true;
    if (worker.joinable()) worker.join();
    ready.notify_all();
  }
  bool read(FramePtr& frame,int timeout_ms) {
    if (timeout_ms<0) throw std::invalid_argument("read timeout cannot be negative");
    std::unique_lock<std::mutex> lock(mutex);
    if (!ever_started) throw std::logic_error("Call Camera::start before Camera::read");
    frame.reset();
    ready.wait_for(lock,std::chrono::milliseconds(timeout_ms),[this] {return !queue.empty() || finished;});
    if (!queue.empty()) {frame=queue.front();queue.pop_front();++stats.delivered;return true;}
    if (!stats.error.empty()) throw std::runtime_error(stats.error);
    return false;
  }
  void update_sdk_statistics() {
    tSdkFrameStatistic counters{};
    const int status=CameraGetFrameStatistic(handle,&counters);
    UINT resends=0;const int resend_status=CameraGetStatisticResend(handle,&resends);
    std::uint64_t udp=0;const bool udp_valid=camera_transport::udpReceiveErrors(udp);
    std::lock_guard<std::mutex> lock(mutex);
    stats.sdk_statistics_valid=status==0;stats.sdk_resends_valid=resend_status==0;
    if (!status) {stats.sdk_capture_count=counters.iCapture;stats.sdk_lost_count=counters.iLost;}
    if (!resend_status) stats.sdk_resend_count=resends;
    stats.host_udp_counter_valid=udp_valid;stats.host_udp_rcvbuf_errors=udp;
  }
  void capture() noexcept {
    bool played=false;
    try {
      check(CameraPlay(handle),"CameraPlay");played=true;
      {std::lock_guard<std::mutex> lock(mutex);startup_complete=true;}ready.notify_all();
      int failures=0;std::uint64_t index=0,last_device_us=0;
      UINT last_id=0;bool have_id=false;
      auto last_statistics=std::chrono::steady_clock::now();
      while (!stopping.load()) {
        BYTE* raw=nullptr;tSdkFrameHead head{};
        const int status=CameraGetImageBufferPriority(handle,&head,&raw,options.timeout_ms,
                                                     CAMERA_GET_IMAGE_PRIORITY_OLDEST);
        const auto host=std::chrono::system_clock::now();
        const auto mono=std::chrono::steady_clock::now();
        if (status!=CAMERA_STATUS_SUCCESS) {
          {std::lock_guard<std::mutex> lock(mutex);++stats.get_buffer_errors;}
          if (stopping) break;
          if (++failures>=10) throw std::runtime_error("10 consecutive SDK retrieval errors; acquisition stopped");
          continue;
        }
        BufferGuard sdk_buffer{handle,raw};failures=0;
        {std::lock_guard<std::mutex> lock(mutex);++stats.captured;}
        if (!raw || head.iWidth!=device.width || head.iHeight!=device.height ||
            head.uiMediaType!=raw_type || head.uBytes!=std::size_t(device.width)*device.height)
          throw std::runtime_error("Raw frame geometry, encoding or payload changed during acquisition");
        Metadata metadata;
        metadata.capture_index=index++;
        metadata.host_unix_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(host.time_since_epoch()).count();
        metadata.host_monotonic_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(mono.time_since_epoch()).count();
        metadata.sdk_timestamp_0p1ms=head.uiTimeStamp;metadata.exposure_us=head.uiExpTime;
        UINT id=0,low=0,high=0;
        metadata.device_frame_id_valid=CameraGetFrameID(handle,&id)==0;metadata.device_frame_id=id;
        metadata.device_timestamp_valid=CameraGetFrameTimeStamp(handle,&low,&high)==0;
        metadata.device_timestamp_us=(std::uint64_t(high)<<32)|low;
        {
          std::lock_guard<std::mutex> lock(mutex);
          if (metadata.device_timestamp_valid) ++stats.valid_device_timestamps;
          if (metadata.device_frame_id_valid) {
            ++stats.valid_frame_ids;
            if (have_id) {
              const bool wrap=metadata.device_timestamp_valid && camera_transport::legacyGvspWrap(
                last_id,id,last_device_us,metadata.device_timestamp_us);
              const UINT delta=wrap?0xffff-last_id+id:id-last_id;
              if (wrap) ++stats.frame_id_wraps;
              if (delta==0) ++stats.frame_id_duplicates;
              else if (delta>=0x80000000U) ++stats.frame_id_resets;
              else if (delta>1) stats.frame_id_gaps+=delta-1;
            }
            last_id=id;have_id=true;
            last_device_us=metadata.device_timestamp_valid?metadata.device_timestamp_us:0;
          } else {have_id=false;last_device_us=0;}
        }
        std::shared_ptr<Frame> frame;
        for (const auto& candidate:pool) if (candidate.use_count()==1) {frame=candidate;break;}
        if (!frame) {
          {std::lock_guard<std::mutex> lock(mutex);++stats.pool_exhaustions;}
          throw std::runtime_error("Frame pool exhausted: consumer retains too many frames; recording is incomplete");
        }
        frame->metadata=metadata;
        std::memcpy(frame->pixels.data(),raw,head.uBytes);
        sdk_buffer.release(); // No disk IO, callbacks or image conversion while SDK owns the buffer.
        {
          std::lock_guard<std::mutex> lock(mutex);
          if (queue.size()>=std::size_t(options.queue_capacity)) {
            ++stats.queue_overruns;
            throw std::runtime_error("Capture queue full: consumer is slower than camera; recording is incomplete");
          }
          queue.push_back(std::move(frame));++stats.enqueued;
        }
        ready.notify_all();
        if (std::chrono::steady_clock::now()-last_statistics>=std::chrono::seconds(1)) {
          update_sdk_statistics();last_statistics=std::chrono::steady_clock::now();
        }
      }
      update_sdk_statistics();
    } catch (const std::exception& error) {
      std::lock_guard<std::mutex> lock(mutex);stats.error=error.what();
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex);stats.error="Unknown capture error";
    }
    if (played) CameraPause(handle);
    {std::lock_guard<std::mutex> lock(mutex);startup_complete=true;finished=true;}
    ready.notify_all();
  }

  Options options;int handle=-1;UINT raw_type=0;
  DeviceInfo device;Statistics stats;
  std::vector<std::shared_ptr<Frame>> pool;
  std::deque<FramePtr> queue;
  mutable std::mutex mutex;std::condition_variable ready;
  std::thread worker;std::atomic<bool> stopping{false};
  bool ever_started=false,startup_complete=false,finished=false;
};

Camera::Camera(const Options& options):impl_(new Impl(options)) {}
Camera::~Camera()=default;
void Camera::start() {impl_->start();}
bool Camera::read(FramePtr& frame,int timeout_ms) {return impl_->read(frame,timeout_ms);}
void Camera::stop() {impl_->stop();}
DeviceInfo Camera::info() const {return impl_->device;}
Statistics Camera::statistics() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->stats;
}
} // namespace xg170gc
