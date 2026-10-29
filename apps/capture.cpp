#include "xg170gc/camera.hpp"
#include <chrono>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
volatile std::sig_atomic_t interrupted=0;
void signal_handler(int) {interrupted=1;}
std::string quoted(const std::string& value) {
  std::ostringstream out;out<<'"';
  for (unsigned char ch:value) {
    if (ch=='"' || ch=='\\') out<<'\\'<<char(ch);
    else if (ch<32) out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<int(ch)<<std::dec;
    else out<<char(ch);
  }
  out<<'"';return out.str();
}
std::string summary(const xg170gc::DeviceInfo& info,const xg170gc::Statistics& s,
                    std::uint64_t consumed,std::uint64_t written,std::uint64_t bytes,
                    std::uint64_t first_ns,std::uint64_t last_ns,const std::string& error,bool discard) {
  std::ostringstream out;out<<std::setprecision(12);
  out<<"{\n  \"model\": "<<quoted(info.model)<<",\n  \"name\": "<<quoted(info.name)
     <<",\n  \"sdk_version\": "<<quoted(info.sdk_version)
     <<",\n  \"vendor_library_modified\": false,\n  \"encoding\": "<<quoted(info.encoding)
     <<",\n  \"width\": "<<info.width<<",\n  \"height\": "<<info.height
     <<",\n  \"exposure_us\": "<<info.exposure_us
     <<",\n  \"host_rmem_max\": "<<info.host_rmem_max
     <<",\n  \"data_sockets_found\": "<<info.data_sockets_found
     <<",\n  \"effective_socket_buffer_bytes\": "<<info.minimum_effective_socket_buffer
     <<",\n  \"captured\": "<<s.captured<<",\n  \"enqueued\": "<<s.enqueued
     <<",\n  \"delivered\": "<<s.delivered<<",\n  \"consumed\": "<<consumed
     <<",\n  \"saved_frames\": "<<written<<",\n  \"saved_raw_bytes\": "<<bytes
     <<",\n  \"discard_mode\": "<<(discard?"true":"false")
     <<",\n  \"frame_id_gaps\": "<<s.frame_id_gaps
     <<",\n  \"frame_id_wraps\": "<<s.frame_id_wraps
     <<",\n  \"frame_id_resets\": "<<s.frame_id_resets
     <<",\n  \"frame_id_duplicates\": "<<s.frame_id_duplicates
     <<",\n  \"valid_frame_ids\": "<<s.valid_frame_ids
     <<",\n  \"valid_device_timestamps\": "<<s.valid_device_timestamps
     <<",\n  \"queue_overruns\": "<<s.queue_overruns
     <<",\n  \"pool_exhaustions\": "<<s.pool_exhaustions
     <<",\n  \"get_buffer_errors\": "<<s.get_buffer_errors
     <<",\n  \"sdk_statistics_valid\": "<<(s.sdk_statistics_valid?"true":"false")
     <<",\n  \"sdk_capture_count\": "<<s.sdk_capture_count
     <<",\n  \"sdk_lost_count\": "<<s.sdk_lost_count
     <<",\n  \"sdk_resends_valid\": "<<(s.sdk_resends_valid?"true":"false")
     <<",\n  \"sdk_resend_count\": "<<s.sdk_resend_count
     <<",\n  \"host_udp_counter_valid\": "<<(s.host_udp_counter_valid?"true":"false")
     <<",\n  \"host_udp_rcvbuf_errors_global\": "<<s.host_udp_rcvbuf_errors
     <<",\n  \"interrupted\": "<<(interrupted?"true":"false")
     <<",\n  \"error\": "<<quoted(error);
  const double span=last_ns>first_ns?double(last_ns-first_ns)*1e-9:0;
  out<<",\n  \"observed_span_s\": "<<span<<",\n  \"consumed_hz\": ";
  if (span>0 && consumed>1) out<<(consumed-1)/span;else out<<"null";
  const bool complete=error.empty() && consumed>0 && s.captured==consumed &&
    s.captured==s.valid_frame_ids && !s.frame_id_gaps && !s.frame_id_resets &&
    !s.frame_id_duplicates && !s.queue_overruns && !s.pool_exhaustions &&
    !s.get_buffer_errors && (discard || written==consumed);
  out<<",\n  \"observed_sequence_complete\": "<<(complete?"true":"false")
     <<",\n  \"scope\": \"Only the observed recording span; no perpetual loss-free guarantee\"\n}\n";
  return out.str();
}
} // namespace

int main(int argc,char** argv) {
  try {
    xg170gc::Options options;double seconds=10;bool discard=false;
    std::string output;
    for (int i=1;i<argc;++i) {
      const std::string arg=argv[i];
      if (arg=="--help" || arg=="-h") {
        std::cout<<"Usage: xg170gc-capture (--output NEW_DIRECTORY | --discard) [options]\n"
          "  --seconds N        acquisition duration (default 10, maximum 3600)\n"
          "  --serial SERIAL    select one camera; default first enumerated\n"
          "  --exposure-us N    manual exposure (default 1000)\n"
          "  --gain N           SDK analog gain units (default 144)\n"
          "  --width N --height N   native ROI; 0/0 means full sensor\n"
          "  --offset-x N --offset-y N   sensor ROI offset\n"
          "  --sdk-buffers N    SDK frame buffers (default 64)\n"
          "  --queue-capacity N consumer FIFO frames (default 32)\n"
          "No ROS, image conversion, display or compressed video encoding.\n";
        return 0;
      }
      if (arg=="--discard") {discard=true;continue;}
      if (i+1>=argc) throw std::invalid_argument("Missing value for "+arg);
      std::string value=argv[++i];
      if (arg=="--seconds") seconds=std::stod(value);
      else if (arg=="--output") output=value;
      else if (arg=="--serial") options.serial=value;
      else if (arg=="--exposure-us") options.exposure_us=std::stod(value);
      else if (arg=="--gain") options.analog_gain=std::stoi(value);
      else if (arg=="--width") options.width=std::stoi(value);
      else if (arg=="--height") options.height=std::stoi(value);
      else if (arg=="--offset-x") options.offset_x=std::stoi(value);
      else if (arg=="--offset-y") options.offset_y=std::stoi(value);
      else if (arg=="--sdk-buffers") options.sdk_buffers=std::stoi(value);
      else if (arg=="--queue-capacity") options.queue_capacity=std::stoi(value);
      else throw std::invalid_argument("Unknown option: "+arg);
    }
    if (!std::isfinite(seconds) || seconds<=0 || seconds>3600 || discard==!output.empty())
      throw std::invalid_argument("Choose exactly one of --output NEW_DIRECTORY or --discard; seconds must be 0..3600");
    std::ofstream raw,csv;
    if (!discard) {
      if (!std::filesystem::create_directory(output))
        throw std::runtime_error("Output directory already exists; refusing to overwrite");
      raw.exceptions(std::ios::badbit|std::ios::failbit);csv.exceptions(std::ios::badbit|std::ios::failbit);
      raw.open(std::filesystem::path(output)/"frames.raw",std::ios::binary);
      csv.open(std::filesystem::path(output)/"frames.csv");
      csv<<"capture_index,device_frame_id,device_frame_id_valid,device_timestamp_us,device_timestamp_valid,host_unix_ns,host_monotonic_ns,sdk_timestamp_0p1ms,exposure_us,width,height,media_type,encoding,byte_offset,payload_bytes\n";
    }
    std::signal(SIGINT,signal_handler);std::signal(SIGTERM,signal_handler);
    xg170gc::Camera camera(options);
    const auto info=camera.info();
    std::cerr<<info.name<<" / "<<info.model<<" SDK "<<info.sdk_version<<" "
             <<info.width<<"x"<<info.height<<" "<<info.encoding<<"\n";
    std::uint64_t consumed=0,written=0,bytes=0,first_ns=0,last_ns=0;
    auto started=std::chrono::steady_clock::now(),last_status=started;
    std::string error;
    auto consume=[&](const xg170gc::FramePtr& frame) {
      const auto& m=frame->metadata;
      if (!consumed) first_ns=m.host_monotonic_ns;
      last_ns=m.host_monotonic_ns;++consumed;
      if (!discard) {
        raw.write(reinterpret_cast<const char*>(frame->pixels.data()),frame->pixels.size());
        csv<<m.capture_index<<','<<m.device_frame_id<<','<<m.device_frame_id_valid<<','
           <<m.device_timestamp_us<<','<<m.device_timestamp_valid<<','<<m.host_unix_ns<<','
           <<m.host_monotonic_ns<<','<<m.sdk_timestamp_0p1ms<<','<<m.exposure_us<<','
           <<frame->width<<','<<frame->height<<','<<frame->media_type<<','<<frame->encoding<<','
           <<bytes<<','<<frame->pixels.size()<<'\n';
        bytes+=frame->pixels.size();++written;
      }
    };
    try {
      camera.start();
      while (!interrupted && std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()<seconds) {
        xg170gc::FramePtr frame;
        if (camera.read(frame,100)) consume(frame);
        const auto now=std::chrono::steady_clock::now();
        if (now-last_status>=std::chrono::seconds(1)) {
          auto s=camera.statistics();
          std::cerr<<"captured="<<s.captured<<" consumed="<<consumed<<" device_gaps="<<s.frame_id_gaps
                   <<" queue_overruns="<<s.queue_overruns<<"\n";last_status=now;
        }
      }
      camera.stop();
      // Drain all frames captured before stop. Never trim the end to hide gaps.
      xg170gc::FramePtr frame;
      while (camera.read(frame,0)) consume(frame);
      if (!discard) {raw.flush();csv.flush();raw.close();csv.close();}
    } catch (const std::exception& failure) {camera.stop();error=failure.what();}
    const auto s=camera.statistics();
    if (error.empty()) error=s.error;
    const auto report=summary(info,s,consumed,written,bytes,first_ns,last_ns,error,discard);
    if (!discard) {
      std::ofstream json(std::filesystem::path(output)/"summary.json");
      json.exceptions(std::ios::badbit|std::ios::failbit);json<<report;json.close();
    }
    std::cout<<report;
    if (!error.empty()) return 1;
    if (interrupted) return 130;
    return report.find("\"observed_sequence_complete\": true")!=std::string::npos?0:3;
  } catch (const std::exception& failure) {std::cerr<<failure.what()<<'\n';return 1;}
}
