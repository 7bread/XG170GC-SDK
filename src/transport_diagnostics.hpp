#pragma once
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <sys/socket.h>
#include <netinet/in.h>

namespace camera_transport {
// Host-wide kernel counter, not a per-camera or a lost-image counter.
inline bool udpReceiveErrors(std::istream& input, std::uint64_t& value) {
  std::string header, values;
  while (std::getline(input,header)) {
    if (header.rfind("Udp:",0)!=0) continue;
    if (!std::getline(input,values) || values.rfind("Udp:",0)!=0) return false;
    std::istringstream keys(header), numbers(values);
    std::string key, label;
    keys>>label; numbers>>label;
    std::uint64_t number;
    while (keys>>key) {
      if (!(numbers>>number)) return false;
      if (key=="RcvbufErrors") {value=number;return true;}
    }
    return false;
  }
  return false;
}
inline bool udpReceiveErrors(std::uint64_t& value) {
  std::ifstream input("/proc/net/snmp");
  return udpReceiveErrors(input,value);
}
inline std::uint64_t receiveBufferLimit() {
  std::uint64_t value=0;
  std::ifstream input("/proc/sys/net/core/rmem_max");
  input>>value; return value;
}
inline bool validOptions(int buffers,int packet) {
  return buffers>=0 && buffers<=128 && (packet==0 || (packet>=1500 && packet<=9000));
}
inline bool legacyGvspWrap(std::uint32_t previous,std::uint32_t current,
                           std::uint64_t previous_us,std::uint64_t current_us) {
  // This camera's GVSP block IDs are 1..65535; zero is reserved. Require a
  // forward device clock to avoid interpreting an actual reset as a wrap.
  return previous>0xf000 && previous<=0xffff && current>0 && current<0x1000 && current_us>previous_us;
}
struct SocketBuffer {int fd=-1,before=0,after=0,error=0;};
// The installed MVSDK requests only 8MiB itself. Adjust only its current
// process's bound UDP/3959 image-data sockets. Never touch ROS/control sockets.
// Linux doubles SO_RCVBUF for bookkeeping; 'after' is this effective value.
inline std::vector<SocketBuffer> enlargeDataSockets(int requested) {
  std::vector<SocketBuffer> result;
  if (!requested) return result;
  DIR* dir=opendir("/proc/self/fd");
  if (!dir) return result;
  while (dirent* entry=readdir(dir)) {
    char* end=nullptr;long number=std::strtol(entry->d_name,&end,10);
    if (!end || *end || number<0) continue;
    int fd=static_cast<int>(number),type=0;socklen_t size=sizeof(type);
    if (getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&size) || type!=SOCK_DGRAM) continue;
    sockaddr_in addr{};size=sizeof(addr);
    if (getsockname(fd,reinterpret_cast<sockaddr*>(&addr),&size) ||
        addr.sin_family!=AF_INET || ntohs(addr.sin_port)!=3959 || addr.sin_addr.s_addr==INADDR_ANY) continue;
    SocketBuffer r;r.fd=fd;size=sizeof(r.before);
    if (getsockopt(fd,SOL_SOCKET,SO_RCVBUF,&r.before,&size)) {r.error=errno;result.push_back(r);continue;}
    if (r.before<2LL*requested && setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&requested,sizeof(requested))) r.error=errno;
    size=sizeof(r.after);
    if (getsockopt(fd,SOL_SOCKET,SO_RCVBUF,&r.after,&size)) r.error=errno;
    result.push_back(r);
  }
  closedir(dir);return result;
}
}
