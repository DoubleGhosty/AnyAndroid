// Sockets of libcore (libcore.io.Linux: libcore_io_Linux.cpp and
// NetworkUtilities.cpp of android-17.0.0_r1) on Winsock. A socket is an open
// file of the process's Vfs: read, write, close, fstat, fcntl, ioctl and poll
// reach it like any descriptor. Its Winsock handle stays non-blocking; a
// blocking call waits in WSAPoll with the GIL released, by slices, so that a
// close from another thread (AsynchronousCloseMonitor) or a timeout
// (SO_RCVTIMEO, SO_SNDTIMEO) ends the wait as in Linux. Constants and errno
// values are bionic's (Linux x86_64), translated both ways.
//
// Without android.permission.INTERNET, Android leaves the process out of the
// inet group: Internet sockets fail with EACCES and name lookups with
// EAI_NODATA caused by EPERM. Names are looked up in the image's
// /system/etc/hosts first, as bionic's files lookup does (localhost is
// 127.0.0.1), then by the host's resolver.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "common/text.h"
#include "framework-bridge/internal.h"
#include "vfs/vfs.h"

// Winsock values older MinGW headers lack.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#ifndef WSA_FLAG_NO_HANDLE_INHERIT
#define WSA_FLAG_NO_HANDLE_INHERIT 0x80
#endif
#ifndef TCP_MAXSEG
#define TCP_MAXSEG 4
#endif
#ifndef TCP_KEEPIDLE
#define TCP_KEEPIDLE 3
#endif
#ifndef TCP_KEEPCNT
#define TCP_KEEPCNT 16
#endif
#ifndef TCP_KEEPINTVL
#define TCP_KEEPINTVL 17
#endif
#ifndef IPV6_TCLASS
#define IPV6_TCLASS 39
#endif

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = rt::ApiStatus;

// Linux (bionic x86_64) values, as the Java side sees them.
namespace lx {
constexpr int32_t kAfUnspec = 0, kAfUnix = 1, kAfInet = 2, kAfInet6 = 10;
constexpr int32_t kSockStream = 1, kSockDgram = 2, kSockRaw = 3, kSockNonblock = 04000, kSockCloexec = 02000000;
constexpr int32_t kSolSocket = 1, kIpprotoIp = 0, kIpprotoTcp = 6, kIpprotoIpv6 = 41;
constexpr int32_t kSoReuseaddr = 2, kSoType = 3, kSoError = 4, kSoDontroute = 5, kSoBroadcast = 6, kSoSndbuf = 7,
                  kSoRcvbuf = 8, kSoKeepalive = 9, kSoOobinline = 10, kSoLinger = 13, kSoReuseport = 15,
                  kSoRcvlowat = 18, kSoSndlowat = 19, kSoRcvtimeo = 20, kSoSndtimeo = 21, kSoAcceptconn = 30,
                  kSoProtocol = 38, kSoDomain = 39;
constexpr int32_t kTcpNodelay = 1, kTcpMaxseg = 2, kTcpKeepidle = 4, kTcpKeepintvl = 5, kTcpKeepcnt = 6;
constexpr int32_t kIpTos = 1, kIpTtl = 2, kIpMulticastTtl = 33, kIpMulticastLoop = 34, kIpMulticastAll = 49;
constexpr int32_t kIpv6UnicastHops = 16, kIpv6MulticastHops = 18, kIpv6MulticastLoop = 19, kIpv6V6only = 26,
                  kIpv6Tclass = 67;
constexpr int16_t kPollIn = 0x1, kPollOut = 0x4, kPollErr = 0x8, kPollHup = 0x10, kPollNval = 0x20,
                  kPollRdnorm = 0x40, kPollWrnorm = 0x100;
constexpr int32_t kMsgOob = 1, kMsgPeek = 2, kMsgDontroute = 4, kMsgDontwait = 0x40;
enum : int32_t {
  kEPERM = 1, kEINTR = 4, kEIO = 5, kEBADF = 9, kEAGAIN = 11, kENOMEM = 12, kEACCES = 13, kEFAULT = 14,
  kEINVAL = 22, kEMFILE = 24, kEPIPE = 32, kEDOM = 33, kENOTSOCK = 88, kEDESTADDRREQ = 89, kEMSGSIZE = 90,
  kEPROTOTYPE = 91, kENOPROTOOPT = 92, kEPROTONOSUPPORT = 93, kESOCKTNOSUPPORT = 94, kEOPNOTSUPP = 95,
  kEPFNOSUPPORT = 96, kEAFNOSUPPORT = 97, kEADDRINUSE = 98, kEADDRNOTAVAIL = 99, kENETDOWN = 100,
  kENETUNREACH = 101, kENETRESET = 102, kECONNABORTED = 103, kECONNRESET = 104, kENOBUFS = 105, kEISCONN = 106,
  kENOTCONN = 107, kETOOMANYREFS = 109, kETIMEDOUT = 110, kECONNREFUSED = 111, kEHOSTDOWN = 112,
  kEHOSTUNREACH = 113, kEALREADY = 114, kEINPROGRESS = 115,
};
// netdb.h
constexpr int32_t kEaiAgain = 2, kEaiBadflags = 3, kEaiFail = 4, kEaiFamily = 5, kEaiMemory = 6, kEaiNodata = 7,
                  kEaiNoname = 8, kEaiService = 9, kEaiSocktype = 10, kEaiOverflow = 14, kEaiMax = 15;
}  // namespace lx

// The Linux errno of a Winsock error. `sending`: a send, which Linux fails
// with EPIPE once the connection is shut down for writing.
int32_t linux_errno(int wsa, bool sending = false) {
  switch (wsa) {
    case WSAEINTR: return lx::kEINTR;
    case WSAEBADF: return lx::kEBADF;
    case WSAEACCES: return lx::kEACCES;
    case WSAEFAULT: return lx::kEFAULT;
    case WSAEINVAL: return lx::kEINVAL;
    case WSAEMFILE: return lx::kEMFILE;
    case WSAEWOULDBLOCK: return lx::kEAGAIN;
    case WSAEINPROGRESS: return lx::kEINPROGRESS;
    case WSAEALREADY: return lx::kEALREADY;
    case WSAENOTSOCK: return lx::kENOTSOCK;
    case WSAEDESTADDRREQ: return lx::kEDESTADDRREQ;
    case WSAEMSGSIZE: return lx::kEMSGSIZE;
    case WSAEPROTOTYPE: return lx::kEPROTOTYPE;
    case WSAENOPROTOOPT: return lx::kENOPROTOOPT;
    case WSAEPROTONOSUPPORT: return lx::kEPROTONOSUPPORT;
    case WSAESOCKTNOSUPPORT: return lx::kESOCKTNOSUPPORT;
    case WSAEOPNOTSUPP: return lx::kEOPNOTSUPP;
    case WSAEPFNOSUPPORT: return lx::kEPFNOSUPPORT;
    case WSAEAFNOSUPPORT: return lx::kEAFNOSUPPORT;
    case WSAEADDRINUSE: return lx::kEADDRINUSE;
    case WSAEADDRNOTAVAIL: return lx::kEADDRNOTAVAIL;
    case WSAENETDOWN: return lx::kENETDOWN;
    case WSAENETUNREACH: return lx::kENETUNREACH;
    case WSAENETRESET: return lx::kENETRESET;
    case WSAECONNABORTED: return lx::kECONNABORTED;
    case WSAECONNRESET: return lx::kECONNRESET;
    case WSAENOBUFS: return lx::kENOBUFS;
    case WSAEISCONN: return lx::kEISCONN;
    case WSAENOTCONN: return lx::kENOTCONN;
    case WSAESHUTDOWN: return sending ? lx::kEPIPE : lx::kENOTCONN;
    case WSAETOOMANYREFS: return lx::kETOOMANYREFS;
    case WSAETIMEDOUT: return lx::kETIMEDOUT;
    case WSAECONNREFUSED: return lx::kECONNREFUSED;
    case WSAEHOSTDOWN: return lx::kEHOSTDOWN;
    case WSAEHOSTUNREACH: return lx::kEHOSTUNREACH;
    case WSA_NOT_ENOUGH_MEMORY: return lx::kENOMEM;
    default: return lx::kEIO;
  }
}

// bionic's EAI_* value of a getaddrinfo/getnameinfo failure of Windows.
int32_t linux_eai(int rc) {
  switch (rc) {
    case WSATRY_AGAIN: return lx::kEaiAgain;
    case WSAEINVAL: return lx::kEaiBadflags;
    case WSANO_RECOVERY: return lx::kEaiFail;
    case WSAEAFNOSUPPORT: return lx::kEaiFamily;
    case WSA_NOT_ENOUGH_MEMORY: return lx::kEaiMemory;
    case WSANO_DATA: return lx::kEaiNodata;
    case WSAHOST_NOT_FOUND: return lx::kEaiNoname;
    case WSATYPE_NOT_FOUND: return lx::kEaiService;
    case WSAESOCKTNOSUPPORT: return lx::kEaiSocktype;
    case WSAEFAULT: return lx::kEaiOverflow;
    default: return lx::kEaiFail;
  }
}

// bionic's gai_strerror (libc/dns/net/getaddrinfo.c).
const char* gai_text(int32_t error) {
  static const char* const kTexts[] = {
      "Success",
      "Address family for hostname not supported",
      "Temporary failure in name resolution",
      "Invalid value for ai_flags",
      "Non-recoverable failure in name resolution",
      "ai_family not supported",
      "Memory allocation failure",
      "No address associated with hostname",
      "hostname nor servname provided, or not known",
      "servname not supported for ai_socktype",
      "ai_socktype not supported",
      "System error returned in errno",
      "Invalid value for hints",
      "Resolved protocol is unknown",
      "Argument buffer overflow",
      "Unknown error",
  };
  if (error < 0 || error > lx::kEaiMax) error = lx::kEaiMax;
  return kTexts[error];
}

int win_family(int32_t linux_family) {
  return linux_family == lx::kAfInet ? AF_INET : linux_family == lx::kAfInet6 ? AF_INET6 : linux_family == 0 ? AF_UNSPEC : -1;
}

int32_t linux_family(int win) { return win == AF_INET ? lx::kAfInet : win == AF_INET6 ? lx::kAfInet6 : win; }

void ensure_winsock() {
  static const bool started = [] {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  (void)started;
}

// A socket of the emulated process. Reads and writes reaching it through the
// Vfs directly (other paths than libcore's descriptor natives) never block.
class NetSocket final : public vfs::OpenFile {
 public:
  NetSocket(SOCKET handle, int32_t family_, int32_t type_, int32_t protocol_, int win_af_)
      : family(family_), type(type_), protocol(protocol_), win_af(win_af_), ino(next_ino()), handle_(handle) {}
  ~NetSocket() override { closesocket(handle_); }
  NetSocket(const NetSocket&) = delete;
  NetSocket& operator=(const NetSocket&) = delete;

  vfs::VStat stat() override {
    vfs::VStat st;
    st.type = vfs::NodeType::kSocket;
    st.mode = 0140777;  // S_IFSOCK, as the kernel reports sockets
    st.ino = ino;
    return st;
  }
  size_t read(uint64_t, std::span<uint8_t> out) override {
    int n = recv(handle_, reinterpret_cast<char*>(out.data()), int(std::min<size_t>(out.size(), INT32_MAX)), 0);
    if (n == SOCKET_ERROR) throw vfs::VfsError{linux_errno(WSAGetLastError()), "read"};
    return size_t(n);
  }
  size_t write(uint64_t, std::span<const uint8_t> data) override {
    int n = send(handle_, reinterpret_cast<const char*>(data.data()), int(std::min<size_t>(data.size(), INT32_MAX)), 0);
    if (n == SOCKET_ERROR) throw vfs::VfsError{linux_errno(WSAGetLastError(), true), "write"};
    return size_t(n);
  }
  void truncate(uint64_t) override { throw vfs::VfsError{lx::kEINVAL, "ftruncate"}; }
  bool seekable() const override { return false; }

  SOCKET handle() const { return handle_; }

  const int32_t family, type, protocol;  // Linux values
  // The Winsock family: AF_INET for an AF_INET6 socket of a host without
  // IPv6, which then takes IPv4 and IPv4-mapped addresses only.
  const int win_af;
  const uint64_t ino;
  std::atomic<bool> nonblocking{false};  // O_NONBLOCK of the open file
  std::atomic<bool> connecting{false};   // a non-blocking connect in progress
  // Options Linux keeps itself: the timeouts of blocking calls (Winsock's
  // would leave the connection in an unspecified state), and SO_REUSEADDR
  // of TCP sockets (Windows' version lets another socket take the port).
  std::atomic<int64_t> rcvtimeo_us{0}, sndtimeo_us{0};
  int32_t reuseaddr = 0;
  // IP_MULTICAST_ALL: Windows never hands a socket the datagrams of groups
  // it did not join, which is the 0 Android sets; the value is kept.
  int32_t multicast_all = 1;

 private:
  static uint64_t next_ino() {
    static std::atomic<uint64_t> counter{100000};
    return ++counter;
  }
  SOCKET handle_;
};

std::shared_ptr<NetSocket> socket_of(int32_t fd) {
  vfs::Vfs& v = libcore_vfs();
  if (fd < 3 || !v.is_open(fd)) return nullptr;
  return std::dynamic_pointer_cast<NetSocket>(v.open_file(fd));
}

// The socket of a FileDescriptor, else EBADF or ENOTSOCK as Linux reports them.
std::shared_ptr<NetSocket> socket_arg(Runtime& rt, Handle java_fd, const char* function) {
  int32_t fd = fd_of(rt, java_fd);
  if (fd < 0 || (fd > 2 && !libcore_vfs().is_open(fd))) throw_errno(rt, function, lx::kEBADF);
  auto s = socket_of(fd);
  if (!s) throw_errno(rt, function, lx::kENOTSOCK);
  return s;
}

// What a close signalled from another thread throws: SocketException for the
// socket calls (NET_FAILURE_RETRY), InterruptedIOException for read and
// write (IO_FAILURE_RETRY).
[[noreturn]] void throw_closed(Runtime& rt, bool io, const char* function) {
  if (io) {
    Handle e = construct(rt, "Ljava/io/InterruptedIOException;", "(Ljava/lang/String;)V",
                         {rt.new_string_utf8(std::string(function) + " interrupted by close() on another thread")});
    throw rt::JavaThrow{e};
  }
  Handle e = construct(rt, "Ljava/net/SocketException;", "(Ljava/lang/String;)V", {rt.new_string_utf8("Socket closed")});
  throw rt::JavaThrow{e};
}

enum class Wait { kReady, kTimeout, kClosed };

// AsynchronousCloseMonitor: the waits blocked on descriptor numbers, which
// signalBlockedThreads(fd) interrupts. By number, as in libcore: a close
// (closeAndSignalBlockedThreads) or PlainSocketImpl's dup2 of its marker
// descriptor over the socket's number wakes whoever waits on that number.
struct BlockedWait {
  std::vector<int32_t> fds;
  std::atomic<bool> signaled{false};
};

std::mutex& blocked_lock() {
  static auto* m = new std::mutex;  // never destroyed
  return *m;
}

std::vector<BlockedWait*>& blocked_waits() {
  static auto* v = new std::vector<BlockedWait*>;  // never destroyed
  return *v;
}

class WaitRegistration {
 public:
  explicit WaitRegistration(std::vector<int32_t> fds) {
    wait_.fds = std::move(fds);
    std::lock_guard<std::mutex> guard(blocked_lock());
    blocked_waits().push_back(&wait_);
  }
  ~WaitRegistration() {
    std::lock_guard<std::mutex> guard(blocked_lock());
    auto& v = blocked_waits();
    v.erase(std::remove(v.begin(), v.end(), &wait_), v.end());
  }
  WaitRegistration(const WaitRegistration&) = delete;
  WaitRegistration& operator=(const WaitRegistration&) = delete;
  bool signaled() const { return wait_.signaled; }

 private:
  BlockedWait wait_;
};

// The socket's pending error (SO_ERROR), as Winsock reports it.
int pending_error(SOCKET s) {
  int err = 0;
  int len = sizeof err;
  if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) == SOCKET_ERROR) return WSAGetLastError();
  return err;
}

// Waits, the GIL released, until the socket is ready for `events`
// (POLLRDNORM, POLLWRNORM), fails or hangs up, a close is signalled from
// another thread, or the timeout (microseconds; 0: none) expires. A failed
// connect is also seen through SO_ERROR, which WSAPoll does not report on
// every Windows version.
Wait wait_socket(Runtime& rt, NetSocket& s, int32_t fd, short events, int64_t timeout_us) {
  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + std::chrono::microseconds(timeout_us);
  Wait result = Wait::kReady;
  WaitRegistration registration({fd});
  rt.run_blocking([&] {
    for (;;) {
      if (registration.signaled()) {
        result = Wait::kClosed;
        return;
      }
      int slice = 50;
      if (timeout_us > 0) {
        auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now()).count();
        if (left <= 0) {
          result = Wait::kTimeout;
          return;
        }
        slice = int(std::min<int64_t>(slice, (left + 999) / 1000));
      }
      WSAPOLLFD p{};
      p.fd = s.handle();
      p.events = events;
      if (WSAPoll(&p, 1, slice) != 0) return;  // ready, failed or hung up: the call says which
      if (s.connecting && pending_error(s.handle()) != 0) return;
    }
  });
  return result;
}

// Runs a socket call until it completes: on a blocking socket it waits for
// `events` meanwhile, at most timeout_us (SO_RCVTIMEO or SO_SNDTIMEO; 0:
// none), then fails with EAGAIN as Linux does. The call returns -1 with the
// Winsock error set on failure.
template <typename Call>
int64_t retry(Runtime& rt, NetSocket& s, int32_t fd, const char* function, short events, int64_t timeout_us,
              bool dontwait, bool io, bool sending, Call call) {
  for (;;) {
    int64_t r = call();
    if (r >= 0) return r;
    int err = WSAGetLastError();
    if (err == WSAEINTR) continue;
    if (err != WSAEWOULDBLOCK || s.nonblocking || dontwait) throw_errno(rt, function, linux_errno(err, sending));
    switch (wait_socket(rt, s, fd, events, timeout_us)) {
      case Wait::kClosed: throw_closed(rt, io, function);
      case Wait::kTimeout: throw_errno(rt, function, lx::kEAGAIN);
      case Wait::kReady: break;
    }
  }
}

int win_msg_flags(int32_t flags) {
  return (flags & lx::kMsgOob ? MSG_OOB : 0) | (flags & lx::kMsgPeek ? MSG_PEEK : 0) |
         (flags & lx::kMsgDontroute ? MSG_DONTROUTE : 0);
}

// --- Addresses (NetworkUtilities.cpp) ------------------------------------------------------

struct Endpoint {
  sockaddr_storage ss{};
  int len = 0;
};

rt::Object& object_of(Runtime& rt, Handle h, const char* what) { return rt.nonnull(h, what); }

uint32_t& field_of(Runtime& rt, Handle h, std::string_view cls, std::string_view name, std::string_view type) {
  rt::Field* f = find_field(rt.find_class(cls), name, type);
  if (!f) rt.throw_new("Ljava/lang/NoSuchFieldError;", std::string(name));
  return object_of(rt, h, "objet").fields.at(f->slot);
}

// inetAddressToSockaddr: InetAddress.holder.family, getAddress() and, for an
// Inet6Address, holder6.scope_id. An Inet4Address becomes an IPv4-mapped
// sockaddr_in6 when `mapped` (sockets of the java.net APIs are AF_INET6).
Endpoint to_sockaddr(Runtime& rt, Handle inet, int32_t port, bool mapped) {
  if (inet == kNull) rt.throw_npe({});
  Handle holder = field_of(rt, inet, "Ljava/net/InetAddress;", "holder", "Ljava/net/InetAddress$InetAddressHolder;");
  int32_t family = int32_t(field_of(rt, holder, "Ljava/net/InetAddress$InetAddressHolder;", "family", "I"));
  Endpoint e;
  if (family == lx::kAfUnspec) {
    e.ss.ss_family = AF_UNSPEC;
    e.len = sizeof e.ss.ss_family;
    return e;
  }
  if (family != lx::kAfInet && family != lx::kAfInet6) {
    rt.throw_new("Ljava/lang/IllegalArgumentException;", "inetAddressToSockaddr bad family: " + std::to_string(family));
  }
  Handle bytes = Handle(rt.call_virtual(inet, "getAddress", "()[B"));
  if (bytes == kNull) rt.throw_npe({});
  rt::ArrayObject& b = rt.array(bytes, "address");
  if (b.length < (family == lx::kAfInet6 ? 16u : 4u)) rt.throw_new("Ljava/lang/IllegalArgumentException;", "adresse tronquée");
  auto& sin6 = reinterpret_cast<sockaddr_in6&>(e.ss);
  if (family == lx::kAfInet6) {
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port = htons(uint16_t(port));
    std::memcpy(&sin6.sin6_addr, b.data.data(), 16);
    Handle holder6 = field_of(rt, inet, "Ljava/net/Inet6Address;", "holder6", "Ljava/net/Inet6Address$Inet6AddressHolder;");
    sin6.sin6_scope_id = field_of(rt, holder6, "Ljava/net/Inet6Address$Inet6AddressHolder;", "scope_id", "I");
    e.len = sizeof(sockaddr_in6);
  } else if (mapped) {
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port = htons(uint16_t(port));
    std::memcpy(&sin6.sin6_addr.s6_addr[12], b.data.data(), 4);
    // INADDR_ANY and in6addr_any are both zeros; other addresses are ::ffff:a.b.c.d.
    if (!IN6_IS_ADDR_UNSPECIFIED(&sin6.sin6_addr)) std::memset(&sin6.sin6_addr.s6_addr[10], 0xff, 2);
    e.len = sizeof(sockaddr_in6);
  } else {
    auto& sin = reinterpret_cast<sockaddr_in&>(e.ss);
    sin.sin_family = AF_INET;
    sin.sin_port = htons(uint16_t(port));
    std::memcpy(&sin.sin_addr, b.data.data(), 4);
    e.len = sizeof(sockaddr_in);
  }
  return e;
}

// sockaddrToInetAddress: InetAddress.getByAddress(null, bytes, scope_id), an
// IPv4-mapped address unmapped first ("Java will never return an IPv4-mapped
// address"); *port set when given.
Handle to_inet_address(Runtime& rt, const sockaddr* sa, int32_t* port) {
  const uint8_t* raw = nullptr;
  size_t length = 0;
  int32_t p = 0;
  uint32_t scope = 0;
  if (sa->sa_family == AF_INET6) {
    const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(sa);
    p = ntohs(sin6->sin6_port);
    if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr)) {
      raw = &sin6->sin6_addr.s6_addr[12];
      length = 4;
    } else {
      raw = sin6->sin6_addr.s6_addr;
      length = 16;
      scope = sin6->sin6_scope_id;
    }
  } else if (sa->sa_family == AF_INET) {
    const auto* sin = reinterpret_cast<const sockaddr_in*>(sa);
    raw = reinterpret_cast<const uint8_t*>(&sin->sin_addr);
    length = 4;
    p = ntohs(sin->sin_port);
  } else {
    rt.throw_new("Ljava/lang/IllegalArgumentException;",
                 "sockaddrToInetAddress unsupported family: " + std::to_string(linux_family(sa->sa_family)));
  }
  if (port) *port = p;
  Handle bytes = rt.new_array("[B", int32_t(length));
  std::memcpy(rt.array(bytes, "address").data.data(), raw, length);
  return Handle(rt.call_static("Ljava/net/InetAddress;", "getByAddress", "(Ljava/lang/String;[BI)Ljava/net/InetAddress;",
                               {kNull, bytes, scope}));
}

constexpr std::string_view kInetSocketAddress = "Ljava/net/InetSocketAddress;";
constexpr std::string_view kIsaHolder = "Ljava/net/InetSocketAddress$InetSocketAddressHolder;";

bool is_inet_socket_address(Runtime& rt, Handle h) { return h != kNull && rt.instance_of(h, rt.find_class(kInetSocketAddress)); }

// makeSocketAddress: an InetSocketAddress for an Internet address.
Handle make_socket_address(Runtime& rt, const sockaddr* sa) {
  if (sa->sa_family != AF_INET && sa->sa_family != AF_INET6) {
    rt.throw_new("Ljava/lang/IllegalArgumentException;", "unsupported ss_family: " + std::to_string(linux_family(sa->sa_family)));
  }
  int32_t port = 0;
  rt::GlobalRef address(rt, to_inet_address(rt, sa, &port));
  return construct(rt, kInetSocketAddress, "(Ljava/net/InetAddress;I)V", {address.get(), uint32_t(port)});
}

// fillInetSocketAddress: the sender's address and port into holder.addr and holder.port.
void fill_inet_socket_address(Runtime& rt, Handle isa, const sockaddr* sa) {
  if (isa == kNull) return;
  int32_t port = 0;
  Handle sender = to_inet_address(rt, sa, &port);
  Handle holder = field_of(rt, isa, kInetSocketAddress, "holder", std::string(kIsaHolder));
  field_of(rt, holder, kIsaHolder, "addr", "Ljava/net/InetAddress;") = sender;
  field_of(rt, holder, kIsaHolder, "port", "I") = uint32_t(port);
}

// javaInetSocketAddressToInetAddressAndPort.
std::pair<Handle, int32_t> inet_and_port(Runtime& rt, Handle isa) {
  Handle holder = field_of(rt, isa, kInetSocketAddress, "holder", std::string(kIsaHolder));
  return {Handle(field_of(rt, holder, kIsaHolder, "addr", "Ljava/net/InetAddress;")),
          int32_t(field_of(rt, holder, kIsaHolder, "port", "I"))};
}

// The address of an InetAddress for a call on socket `s`: IPv4-mapped for an
// AF_INET6 socket, plain IPv4 when its Winsock socket is AF_INET (no IPv6 on
// the host), where an IPv6 address cannot be reached.
Endpoint endpoint_for(Runtime& rt, NetSocket& s, Handle inet, int32_t port, const char* function) {
  Endpoint e = to_sockaddr(rt, inet, port, s.family == lx::kAfInet6 && s.win_af == AF_INET6);
  if (s.win_af != AF_INET || e.ss.ss_family != AF_INET6) return e;
  const auto& sin6 = reinterpret_cast<const sockaddr_in6&>(e.ss);
  bool unspecified = IN6_IS_ADDR_UNSPECIFIED(&sin6.sin6_addr), mapped = IN6_IS_ADDR_V4MAPPED(&sin6.sin6_addr);
  if (!unspecified && !mapped) {
    throw_errno(rt, function, std::string_view(function) == "bind" ? lx::kEADDRNOTAVAIL : lx::kENETUNREACH);
  }
  Endpoint v4;
  auto& sin = reinterpret_cast<sockaddr_in&>(v4.ss);
  sin.sin_family = AF_INET;
  sin.sin_port = sin6.sin6_port;
  if (mapped) std::memcpy(&sin.sin_addr, &sin6.sin6_addr.s6_addr[12], 4);
  v4.len = sizeof(sockaddr_in);
  return v4;
}

// A SocketAddress argument: only Internet addresses (UnixSocketAddress,
// netlink and packet addresses need sockets AnyAndroid does not have).
Endpoint socket_address_arg(Runtime& rt, NetSocket& s, Handle address, const char* function) {
  if (!is_inet_socket_address(rt, address)) {
    if (address == kNull) rt.throw_npe({});
    throw_errno(rt, function, lx::kEAFNOSUPPORT);
  }
  auto [inet, port] = inet_and_port(rt, address);
  return endpoint_for(rt, s, inet, port, function);
}

// --- Names: /system/etc/hosts, then the host's resolver ---------------------------------

// The entries of the image's hosts file: (address, names).
std::vector<std::pair<std::string, std::vector<std::string>>> hosts_entries() {
  std::vector<std::pair<std::string, std::vector<std::string>>> out;
  std::vector<uint8_t> bytes;
  try {
    bytes = libcore_vfs().read_file("/system/etc/hosts", 1u << 20);
  } catch (const vfs::VfsError&) {
    return out;
  }
  std::istringstream lines(std::string(bytes.begin(), bytes.end()));
  std::string line;
  while (std::getline(lines, line)) {
    if (size_t hash = line.find('#'); hash != std::string::npos) line.resize(hash);
    std::istringstream words(line);
    std::string address, name;
    if (!(words >> address)) continue;
    std::vector<std::string> names;
    while (words >> name) names.push_back(name);
    if (!names.empty()) out.emplace_back(address, std::move(names));
  }
  return out;
}

bool parse_address(const std::string& text, int family, sockaddr_storage& ss) {
  std::memset(&ss, 0, sizeof ss);
  if (family != AF_INET6 && InetPtonA(AF_INET, text.c_str(), &reinterpret_cast<sockaddr_in&>(ss).sin_addr) == 1) {
    ss.ss_family = AF_INET;
    return true;
  }
  if (family != AF_INET && InetPtonA(AF_INET6, text.c_str(), &reinterpret_cast<sockaddr_in6&>(ss).sin6_addr) == 1) {
    ss.ss_family = AF_INET6;
    return true;
  }
  return false;
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

std::vector<sockaddr_storage> hosts_lookup(const std::string& node, int family) {
  std::vector<sockaddr_storage> found;
  std::string wanted = lower(node);
  for (const auto& [address, names] : hosts_entries()) {
    bool match = std::any_of(names.begin(), names.end(), [&](const std::string& n) { return lower(n) == wanted; });
    sockaddr_storage ss;
    if (match && parse_address(address, family, ss)) found.push_back(ss);
  }
  return found;
}

std::optional<std::string> hosts_reverse(const sockaddr* sa) {
  for (const auto& [address, names] : hosts_entries()) {
    sockaddr_storage ss;
    if (!parse_address(address, sa->sa_family, ss) || ss.ss_family != sa->sa_family) continue;
    bool same = sa->sa_family == AF_INET
                    ? std::memcmp(&reinterpret_cast<const sockaddr_in*>(sa)->sin_addr,
                                  &reinterpret_cast<sockaddr_in&>(ss).sin_addr, 4) == 0
                    : std::memcmp(&reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr,
                                  &reinterpret_cast<sockaddr_in6&>(ss).sin6_addr, 16) == 0;
    if (same) return names.front();
  }
  return std::nullopt;
}

// throwGaiException: GaiException(function, error), caused by an
// ErrnoException when errno is set (the missing INTERNET permission).
[[noreturn]] void throw_gai(Runtime& rt, const char* function, int32_t error, int32_t cause_errno) {
  Handle name = rt.new_string_utf8(function);
  if (cause_errno == 0) {
    throw rt::JavaThrow{construct(rt, "Landroid/system/GaiException;", "(Ljava/lang/String;I)V", {name, uint32_t(error)})};
  }
  rt::GlobalRef keep(rt, name);
  rt::GlobalRef cause(rt, construct(rt, "Landroid/system/ErrnoException;", "(Ljava/lang/String;I)V",
                                    {name, uint32_t(cause_errno)}));
  throw rt::JavaThrow{construct(rt, "Landroid/system/GaiException;", "(Ljava/lang/String;ILjava/lang/Throwable;)V",
                                {keep.get(), uint32_t(error), cause.get()})};
}

// --- Socket options ----------------------------------------------------------------------

struct WinOption {
  int level;
  int name;
};

// The Winsock option of a Linux level and option, if Winsock has it.
std::optional<WinOption> win_option(int32_t level, int32_t option) {
  switch (level) {
    case lx::kSolSocket:
      switch (option) {
        case lx::kSoReuseaddr: return WinOption{SOL_SOCKET, SO_REUSEADDR};
        case lx::kSoType: return WinOption{SOL_SOCKET, SO_TYPE};
        case lx::kSoError: return WinOption{SOL_SOCKET, SO_ERROR};
        case lx::kSoDontroute: return WinOption{SOL_SOCKET, SO_DONTROUTE};
        case lx::kSoBroadcast: return WinOption{SOL_SOCKET, SO_BROADCAST};
        case lx::kSoSndbuf: return WinOption{SOL_SOCKET, SO_SNDBUF};
        case lx::kSoRcvbuf: return WinOption{SOL_SOCKET, SO_RCVBUF};
        case lx::kSoKeepalive: return WinOption{SOL_SOCKET, SO_KEEPALIVE};
        case lx::kSoOobinline: return WinOption{SOL_SOCKET, SO_OOBINLINE};
        case lx::kSoLinger: return WinOption{SOL_SOCKET, SO_LINGER};
        case lx::kSoRcvlowat: return WinOption{SOL_SOCKET, SO_RCVLOWAT};
        case lx::kSoSndlowat: return WinOption{SOL_SOCKET, SO_SNDLOWAT};
        case lx::kSoAcceptconn: return WinOption{SOL_SOCKET, SO_ACCEPTCONN};
        default: return std::nullopt;
      }
    case lx::kIpprotoTcp:
      switch (option) {
        case lx::kTcpNodelay: return WinOption{IPPROTO_TCP, TCP_NODELAY};
        case lx::kTcpMaxseg: return WinOption{IPPROTO_TCP, TCP_MAXSEG};
        case lx::kTcpKeepidle: return WinOption{IPPROTO_TCP, TCP_KEEPIDLE};
        case lx::kTcpKeepintvl: return WinOption{IPPROTO_TCP, TCP_KEEPINTVL};
        case lx::kTcpKeepcnt: return WinOption{IPPROTO_TCP, TCP_KEEPCNT};
        default: return std::nullopt;
      }
    case lx::kIpprotoIp:
      switch (option) {
        case lx::kIpTos: return WinOption{IPPROTO_IP, IP_TOS};
        case lx::kIpTtl: return WinOption{IPPROTO_IP, IP_TTL};
        case lx::kIpMulticastTtl: return WinOption{IPPROTO_IP, IP_MULTICAST_TTL};
        case lx::kIpMulticastLoop: return WinOption{IPPROTO_IP, IP_MULTICAST_LOOP};
        default: return std::nullopt;
      }
    case lx::kIpprotoIpv6:
      switch (option) {
        case lx::kIpv6UnicastHops: return WinOption{IPPROTO_IPV6, IPV6_UNICAST_HOPS};
        case lx::kIpv6MulticastHops: return WinOption{IPPROTO_IPV6, IPV6_MULTICAST_HOPS};
        case lx::kIpv6MulticastLoop: return WinOption{IPPROTO_IPV6, IPV6_MULTICAST_LOOP};
        case lx::kIpv6V6only: return WinOption{IPPROTO_IPV6, IPV6_V6ONLY};
        case lx::kIpv6Tclass: return WinOption{IPPROTO_IPV6, IPV6_TCLASS};
        default: return std::nullopt;
      }
    default: return std::nullopt;
  }
}

int32_t get_int_option(Runtime& rt, NetSocket& s, int32_t level, int32_t option) {
  if (level == lx::kSolSocket) {
    switch (option) {
      case lx::kSoType: return s.type;
      case lx::kSoDomain: return s.family;
      case lx::kSoProtocol: return s.protocol != 0 ? s.protocol : s.type == lx::kSockStream ? IPPROTO_TCP : IPPROTO_UDP;
      case lx::kSoError: {
        int err = pending_error(s.handle());
        s.connecting = false;
        return err == 0 ? 0 : linux_errno(err);
      }
      case lx::kSoReuseaddr:
        if (s.type == lx::kSockStream) return s.reuseaddr;
        break;
      case lx::kSoReuseport: return 0;
      default: break;
    }
  }
  if (level == lx::kIpprotoIp && option == lx::kIpMulticastAll) return s.multicast_all;
  auto w = win_option(level, option);
  if (!w) throw_errno(rt, "getsockopt", lx::kENOPROTOOPT);
  DWORD value = 0;
  int len = sizeof value;
  if (getsockopt(s.handle(), w->level, w->name, reinterpret_cast<char*>(&value), &len) == SOCKET_ERROR) {
    throw_errno(rt, "getsockopt", linux_errno(WSAGetLastError()));
  }
  if (len == 1) value &= 0xff;  // BOOLEAN-sized options
  return int32_t(value);
}

void set_int_option(Runtime& rt, NetSocket& s, int32_t level, int32_t option, int32_t value) {
  if (level == lx::kSolSocket && option == lx::kSoReuseaddr && s.type == lx::kSockStream) {
    s.reuseaddr = value != 0;  // Linux's rebinding after TIME_WAIT is Windows' default
    return;
  }
  if (level == lx::kSolSocket && option == lx::kSoReuseport) throw_errno(rt, "setsockopt", lx::kENOPROTOOPT);
  if (level == lx::kIpprotoIp && option == lx::kIpMulticastAll) {
    s.multicast_all = value != 0;
    return;
  }
  auto w = win_option(level, option);
  if (!w) throw_errno(rt, "setsockopt", lx::kENOPROTOOPT);
  DWORD v = DWORD(value);
  if (setsockopt(s.handle(), w->level, w->name, reinterpret_cast<const char*>(&v), sizeof v) == SOCKET_ERROR) {
    throw_errno(rt, "setsockopt", linux_errno(WSAGetLastError()));
  }
}

// --- Natives --------------------------------------------------------------------------------

Handle install_socket(Runtime& rt, std::shared_ptr<NetSocket> sock, const char* function) {
  int32_t flags = vfs::kO_RDWR | (sock->nonblocking ? vfs::kO_NONBLOCK : 0);
  std::string path = "socket:[" + std::to_string(sock->ino) + "]";
  int32_t fd;
  try {
    fd = libcore_vfs().install(std::move(sock), flags, std::move(path));
  } catch (const vfs::VfsError& e) {
    throw_errno(rt, function, e.err);
  }
  return new_file_descriptor(rt, fd);
}

// A non-blocking Winsock socket set up as Linux's default: dual-stack IPv6,
// no ECONNRESET from ICMP on unconnected UDP sockets.
SOCKET new_win_socket(int af, int type, int protocol) {
  ensure_winsock();
  SOCKET s = WSASocketW(af, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  if (s == INVALID_SOCKET) return s;
  u_long on = 1;
  ioctlsocket(s, FIONBIO, &on);
  if (af == AF_INET6) {
    DWORD off = 0;
    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof off);
  }
  if (type == SOCK_DGRAM) {
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof report, nullptr, 0, &returned, nullptr, nullptr);
  }
  return s;
}

void do_bind(Runtime& rt, NetSocket& s, const Endpoint& e) {
  if (bind(s.handle(), reinterpret_cast<const sockaddr*>(&e.ss), e.len) == SOCKET_ERROR) {
    throw_errno(rt, "bind", linux_errno(WSAGetLastError()));
  }
}

// connect(2): EINPROGRESS on a non-blocking socket; a blocking one waits
// until the connection is made or fails (Winsock's own timeout).
void do_connect(Runtime& rt, NetSocket& s, int32_t fd, const Endpoint& e) {
  if (connect(s.handle(), reinterpret_cast<const sockaddr*>(&e.ss), e.len) == 0) return;
  int err = WSAGetLastError();
  if (err == WSAEINVAL && s.connecting) err = WSAEALREADY;  // a connect already in progress
  if (err != WSAEWOULDBLOCK) throw_errno(rt, "connect", linux_errno(err));
  s.connecting = true;
  if (s.nonblocking) throw_errno(rt, "connect", lx::kEINPROGRESS);
  if (wait_socket(rt, s, fd, POLLWRNORM, 0) == Wait::kClosed) throw_closed(rt, false, "connect");
  s.connecting = false;
  if (int failed = pending_error(s.handle())) throw_errno(rt, "connect", linux_errno(failed));
}

Handle sock_name(Runtime& rt, Handle java_fd, bool peer) {
  const char* function = peer ? "getpeername" : "getsockname";
  auto s = socket_arg(rt, java_fd, function);
  sockaddr_storage ss{};
  int len = sizeof ss;
  int rc = peer ? getpeername(s->handle(), reinterpret_cast<sockaddr*>(&ss), &len)
                : getsockname(s->handle(), reinterpret_cast<sockaddr*>(&ss), &len);
  if (rc == SOCKET_ERROR) {
    int err = WSAGetLastError();
    // Linux names an unbound socket: the wildcard address, port 0.
    if (peer || err != WSAEINVAL) throw_errno(rt, function, linux_errno(err));
    std::memset(&ss, 0, sizeof ss);
    ss.ss_family = ADDRESS_FAMILY(s->win_af);
  }
  return make_socket_address(rt, reinterpret_cast<const sockaddr*>(&ss));
}

int64_t timeval_us(Runtime& rt, Handle timeval) {
  if (timeval == kNull) rt.throw_npe("null javaTimeval");
  auto read_long = [&](const char* name) {
    rt::Field* f = find_field(rt.class_of(timeval), name, "J");
    rt::Object& o = rt.nonnull(timeval, "StructTimeval");
    return int64_t(uint64_t(o.fields.at(f->slot)) | (uint64_t(o.fields.at(f->slot + 1)) << 32));
  };
  int64_t sec = read_long("tv_sec"), usec = read_long("tv_usec");
  // sock_set_timeout: tv_usec outside [0, 1000000) is EDOM; a negative timeout means none.
  if (usec < 0 || usec >= 1000000) return -1;
  if (sec < 0) return 0;
  return sec * 1000000 + usec;
}

// Linux poll(2) over descriptors of the Vfs: sockets through WSAPoll, the
// other open descriptors always ready (regular files, devices, standard
// streams), closed ones POLLNVAL, negative ones ignored.
struct PollEntry {
  int32_t fd = -1;
  int16_t events = 0;
  int16_t revents = 0;
  std::shared_ptr<NetSocket> sock;
};

int16_t to_wsa_events(int16_t events) {
  return int16_t((events & (lx::kPollIn | lx::kPollRdnorm) ? POLLRDNORM : 0) |
                 (events & (lx::kPollOut | lx::kPollWrnorm) ? POLLWRNORM : 0));
}

int16_t from_wsa_events(int16_t wsa, int16_t wanted) {
  int16_t r = 0;
  if (wsa & (POLLRDNORM | POLLRDBAND)) r |= int16_t(wanted & (lx::kPollIn | lx::kPollRdnorm));
  if (wsa & POLLWRNORM) r |= int16_t(wanted & (lx::kPollOut | lx::kPollWrnorm));
  if (wsa & POLLERR) r |= lx::kPollErr;
  if (wsa & POLLHUP) r |= lx::kPollHup;
  if (wsa & POLLNVAL) r |= lx::kPollNval;
  return r;
}

// One pass over the entries (timeout_ms for WSAPoll); the number of ready ones.
int poll_once(std::vector<PollEntry>& entries, int timeout_ms) {
  std::vector<WSAPOLLFD> fds;
  std::vector<size_t> which;
  int ready = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    PollEntry& e = entries[i];
    if (!e.sock) {
      if (e.revents) ++ready;
      continue;
    }
    WSAPOLLFD p{};
    p.fd = e.sock->handle();
    p.events = to_wsa_events(e.events);
    fds.push_back(p);
    which.push_back(i);
  }
  if (fds.empty()) return ready;
  int rc = WSAPoll(fds.data(), ULONG(fds.size()), ready ? 0 : timeout_ms);
  if (rc < 0) rc = 0;
  for (size_t k = 0; k < fds.size(); ++k) {
    PollEntry& e = entries[which[k]];
    e.revents = from_wsa_events(fds[k].revents, e.events);
    if (!e.revents && e.sock->connecting && pending_error(e.sock->handle()) != 0) {
      e.revents = int16_t((e.events & lx::kPollOut) | lx::kPollErr | lx::kPollHup);
    }
    if (e.revents) ++ready;
  }
  return ready;
}

[[noreturn]] void throw_net(Runtime& rt, const char* cls, const std::string& message) {
  throw rt::JavaThrow{construct(rt, cls, "(Ljava/lang/String;)V", {rt.new_string_utf8(message)})};
}

// The socket of a SocketInputStream or SocketOutputStream: "Socket closed"
// for a null or released descriptor (Bug 4086704 in ojluni).
std::shared_ptr<NetSocket> stream_socket(Runtime& rt, Handle java_fd) {
  if (java_fd == kNull) throw_net(rt, "Ljava/net/SocketException;", "Socket closed");
  int32_t fd = fd_of(rt, java_fd);
  if (fd == -1) throw_net(rt, "Ljava/net/SocketException;", "Socket closed");
  auto s = socket_of(fd);
  if (!s) {
    int32_t err = fd > 2 && libcore_vfs().is_open(fd) ? lx::kENOTSOCK : lx::kEBADF;
    if (err == lx::kEBADF) throw_net(rt, "Ljava/net/SocketException;", "Socket closed");
    throw_net(rt, "Ljava/net/SocketException;", strerror_text(err));
  }
  return s;
}

// A blocking send or recv of the ojluni streams (NET_Read, NET_Send): the
// Linux errno of a failure, EBADF when a close was signalled.
template <typename Call>
std::pair<int64_t, int32_t> stream_io(Runtime& rt, NetSocket& s, int32_t fd, short events, bool sending, Call call) {
  for (;;) {
    int64_t r = call();
    if (r >= 0) return {r, 0};
    int err = WSAGetLastError();
    if (err == WSAEINTR) continue;
    if (err != WSAEWOULDBLOCK || s.nonblocking) return {-1, linux_errno(err, sending)};
    if (wait_socket(rt, s, fd, events, 0) == Wait::kClosed) return {-1, lx::kEBADF};
  }
}

// socketpair(2) of AF_UNIX: two connected TCP (or UDP) sockets of the
// loopback interface, Windows having no socketpair.
std::pair<SOCKET, SOCKET> loopback_pair(int type) {
  sockaddr_in any{};
  any.sin_family = AF_INET;
  any.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  auto fail = [](SOCKET a, SOCKET b, SOCKET c) {
    int err = WSAGetLastError();
    for (SOCKET x : {a, b, c}) {
      if (x != INVALID_SOCKET) closesocket(x);
    }
    WSASetLastError(err);
    return std::pair<SOCKET, SOCKET>{INVALID_SOCKET, INVALID_SOCKET};
  };
  if (type == SOCK_DGRAM) {
    SOCKET a = new_win_socket(AF_INET, SOCK_DGRAM, 0), b = new_win_socket(AF_INET, SOCK_DGRAM, 0);
    if (a == INVALID_SOCKET || b == INVALID_SOCKET) return fail(a, b, INVALID_SOCKET);
    sockaddr_in na{}, nb{};
    int la = sizeof na, lb = sizeof nb;
    if (bind(a, reinterpret_cast<sockaddr*>(&any), sizeof any) || bind(b, reinterpret_cast<sockaddr*>(&any), sizeof any) ||
        getsockname(a, reinterpret_cast<sockaddr*>(&na), &la) || getsockname(b, reinterpret_cast<sockaddr*>(&nb), &lb) ||
        connect(a, reinterpret_cast<sockaddr*>(&nb), lb) || connect(b, reinterpret_cast<sockaddr*>(&na), la)) {
      return fail(a, b, INVALID_SOCKET);
    }
    return {a, b};
  }
  SOCKET listener = new_win_socket(AF_INET, SOCK_STREAM, 0);
  SOCKET client = new_win_socket(AF_INET, SOCK_STREAM, 0);
  if (listener == INVALID_SOCKET || client == INVALID_SOCKET) return fail(listener, client, INVALID_SOCKET);
  sockaddr_in where{};
  int len = sizeof where;
  if (bind(listener, reinterpret_cast<sockaddr*>(&any), sizeof any) || listen(listener, 1) ||
      getsockname(listener, reinterpret_cast<sockaddr*>(&where), &len)) {
    return fail(listener, client, INVALID_SOCKET);
  }
  if (connect(client, reinterpret_cast<sockaddr*>(&where), len) && WSAGetLastError() != WSAEWOULDBLOCK) {
    return fail(listener, client, INVALID_SOCKET);
  }
  WSAPOLLFD p{};
  p.fd = listener;
  p.events = POLLRDNORM;
  if (WSAPoll(&p, 1, 5000) != 1) return fail(listener, client, INVALID_SOCKET);
  SOCKET server = accept(listener, nullptr, nullptr);
  if (server == INVALID_SOCKET) return fail(listener, client, INVALID_SOCKET);
  closesocket(listener);
  u_long on = 1;
  ioctlsocket(server, FIONBIO, &on);
  p.fd = client;
  p.events = POLLWRNORM;
  WSAPoll(&p, 1, 5000);  // the connection completes
  return {server, client};
}

}  // namespace

std::optional<size_t> socket_read(Runtime& rt, int32_t fd, std::span<uint8_t> out) {
  auto s = socket_of(fd);
  if (!s) return std::nullopt;
  int len = int(std::min<size_t>(out.size(), INT32_MAX));
  return size_t(retry(rt, *s, fd, "read", POLLRDNORM, s->rcvtimeo_us, false, true, false, [&]() -> int64_t {
    return recv(s->handle(), reinterpret_cast<char*>(out.data()), len, 0);
  }));
}

std::optional<size_t> socket_write(Runtime& rt, int32_t fd, std::span<const uint8_t> data) {
  auto s = socket_of(fd);
  if (!s) return std::nullopt;
  int len = int(std::min<size_t>(data.size(), INT32_MAX));
  return size_t(retry(rt, *s, fd, "write", POLLWRNORM, s->sndtimeo_us, false, true, true, [&]() -> int64_t {
    return send(s->handle(), reinterpret_cast<const char*>(data.data()), len, 0);
  }));
}

std::optional<int32_t> socket_available(Runtime& rt, int32_t fd) {
  auto s = socket_of(fd);
  if (!s) return std::nullopt;
  u_long n = 0;
  if (ioctlsocket(s->handle(), FIONREAD, &n) == SOCKET_ERROR) throw_errno(rt, "ioctl", linux_errno(WSAGetLastError()));
  return int32_t(std::min<u_long>(n, INT32_MAX));
}

void socket_status_flags(int32_t fd, int32_t flags) {
  if (auto s = socket_of(fd)) s->nonblocking = (flags & vfs::kO_NONBLOCK) != 0;
}

void signal_socket(int32_t fd) {
  std::lock_guard<std::mutex> guard(blocked_lock());
  for (BlockedWait* w : blocked_waits()) {
    if (std::find(w->fds.begin(), w->fds.end(), fd) != w->fds.end()) w->signaled = true;
  }
}

void register_libcore_net(rt::NativeRegistry& r) {
  const char* lxc = "Llibcore/io/Linux;";  // instance methods: argument 0 is the Linux object
  r.bind(lxc, "socket", "(III)Ljava/io/FileDescriptor;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t domain = a.i32(1), type = a.i32(2), protocol = a.i32(3);
    int32_t base = type & ~(lx::kSockNonblock | lx::kSockCloexec);
    int af = win_family(domain);
    if (af < 0 || af == AF_UNSPEC) throw_errno(rt, "socket", lx::kEAFNOSUPPORT);  // AF_UNIX, netlink, packet
    if (!libcore_config().internet) throw_errno(rt, "socket", lx::kEACCES);
    if (base == lx::kSockRaw) throw_errno(rt, "socket", lx::kEPERM);  // no CAP_NET_RAW for applications
    if (base != lx::kSockStream && base != lx::kSockDgram) throw_errno(rt, "socket", lx::kESOCKTNOSUPPORT);
    int wtype = base == lx::kSockStream ? SOCK_STREAM : SOCK_DGRAM;
    SOCKET s = new_win_socket(af, wtype, protocol);
    if (s == INVALID_SOCKET && af == AF_INET6 && WSAGetLastError() == WSAEAFNOSUPPORT) {
      af = AF_INET;  // a host without IPv6: an IPv4 socket behind the AF_INET6 one
      s = new_win_socket(af, wtype, protocol);
    }
    if (s == INVALID_SOCKET) throw_errno(rt, "socket", linux_errno(WSAGetLastError()));
    auto sock = std::make_shared<NetSocket>(s, domain, base, protocol, af);
    sock->nonblocking = (type & lx::kSockNonblock) != 0;
    return install_socket(rt, std::move(sock), "socket");
  }, S::kPartial, "AF_INET et AF_INET6 (TCP, UDP) sur Winsock ; ni AF_UNIX, ni netlink, ni socket brut");
  r.bind(lxc, "bind", "(Ljava/io/FileDescriptor;Ljava/net/InetAddress;I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = socket_arg(rt, a.h(1), "bind");
    do_bind(rt, *s, endpoint_for(rt, *s, a.h(2), a.i32(3), "bind"));
    return 0;
  });
  r.bind(lxc, "bind", "(Ljava/io/FileDescriptor;Ljava/net/SocketAddress;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = socket_arg(rt, a.h(1), "bind");
    do_bind(rt, *s, socket_address_arg(rt, *s, a.h(2), "bind"));
    return 0;
  });
  r.bind(lxc, "listen", "(Ljava/io/FileDescriptor;I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = socket_arg(rt, a.h(1), "listen");
    if (listen(s->handle(), std::max(a.i32(2), 0)) == SOCKET_ERROR) throw_errno(rt, "listen", linux_errno(WSAGetLastError()));
    return 0;
  });
  // The accepted socket is blocking (accept, not accept4), its peer's
  // address written into the InetSocketAddress given.
  r.bind(lxc, "accept", "(Ljava/io/FileDescriptor;Ljava/net/SocketAddress;)Ljava/io/FileDescriptor;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto s = socket_arg(rt, a.h(1), "accept");
           sockaddr_storage ss{};
           int len = sizeof ss;
           SOCKET client = INVALID_SOCKET;
           retry(rt, *s, fd_of(rt, a.h(1)), "accept", POLLRDNORM, s->rcvtimeo_us, false, false, false, [&]() -> int64_t {
             len = sizeof ss;
             client = accept(s->handle(), reinterpret_cast<sockaddr*>(&ss), &len);
             return client == INVALID_SOCKET ? -1 : 0;
           });
           u_long on = 1;
           ioctlsocket(client, FIONBIO, &on);
           auto sock = std::make_shared<NetSocket>(client, s->family, s->type, s->protocol, s->win_af);
           if (Handle peer = a.h(2); peer != kNull) {
             if (!is_inet_socket_address(rt, peer)) {
               rt.throw_new("Ljava/lang/UnsupportedOperationException;", "unsupported SocketAddress subclass");
             }
             fill_inet_socket_address(rt, peer, reinterpret_cast<const sockaddr*>(&ss));
           }
           return install_socket(rt, std::move(sock), "accept");
         });
  r.bind(lxc, "connect", "(Ljava/io/FileDescriptor;Ljava/net/InetAddress;I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = socket_arg(rt, a.h(1), "connect");
    do_connect(rt, *s, fd_of(rt, a.h(1)), endpoint_for(rt, *s, a.h(2), a.i32(3), "connect"));
    return 0;
  });
  r.bind(lxc, "connect", "(Ljava/io/FileDescriptor;Ljava/net/SocketAddress;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = socket_arg(rt, a.h(1), "connect");
    do_connect(rt, *s, fd_of(rt, a.h(1)), socket_address_arg(rt, *s, a.h(2), "connect"));
    return 0;
  });
  r.bind(lxc, "getsockname", "(Ljava/io/FileDescriptor;)Ljava/net/SocketAddress;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t { return sock_name(rt, a.h(1), false); });
  r.bind(lxc, "getpeername", "(Ljava/io/FileDescriptor;)Ljava/net/SocketAddress;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t { return sock_name(rt, a.h(1), true); });
  r.bind(lxc, "shutdown", "(Ljava/io/FileDescriptor;I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = socket_arg(rt, a.h(1), "shutdown");
    if (shutdown(s->handle(), a.i32(2)) == SOCKET_ERROR) throw_errno(rt, "shutdown", linux_errno(WSAGetLastError()));
    return 0;
  });

  // Data: recvfrom and sendto, with a byte[] or a ByteBuffer (Linux.java
  // passes either as an Object).
  auto recvfrom_native = [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = socket_arg(rt, a.h(1), "recvfrom");
    auto buf = extract_io_buffer(rt, a.h(2), a.i32(3), a.i32(4), true);
    int32_t flags = a.i32(5);
    Handle from = a.h(6);
    sockaddr_storage ss{};
    int len = sizeof ss;
    int size = int(std::min<size_t>(buf.size(), INT32_MAX));
    int64_t n = retry(rt, *s, fd_of(rt, a.h(1)), "recvfrom", POLLRDNORM, s->rcvtimeo_us, (flags & lx::kMsgDontwait) != 0, false, false,
                      [&]() -> int64_t {
                        len = sizeof ss;
                        int got = recvfrom(s->handle(), reinterpret_cast<char*>(buf.data()), size, win_msg_flags(flags),
                                           from != kNull ? reinterpret_cast<sockaddr*>(&ss) : nullptr,
                                           from != kNull ? &len : nullptr);
                        // A datagram longer than the buffer: Linux returns what fits.
                        if (got == SOCKET_ERROR && WSAGetLastError() == WSAEMSGSIZE) return size;
                        return got;
                      });
    if (from != kNull && (ss.ss_family == AF_INET || ss.ss_family == AF_INET6)) {
      fill_inet_socket_address(rt, from, reinterpret_cast<const sockaddr*>(&ss));
    }
    return rt::ret_int(int32_t(n));
  };
  r.bind(lxc, "recvfromBytes", "(Ljava/io/FileDescriptor;Ljava/lang/Object;IIILjava/net/InetSocketAddress;)I", recvfrom_native);
  auto send_to = [](Runtime& rt, NetSocket& s, NativeArgs& a, const Endpoint* to) -> uint64_t {
    auto buf = extract_io_buffer(rt, a.h(2), a.i32(3), a.i32(4), false);
    int32_t flags = a.i32(5);
    int size = int(std::min<size_t>(buf.size(), INT32_MAX));
    int64_t n = retry(rt, s, fd_of(rt, a.h(1)), "sendto", POLLWRNORM, s.sndtimeo_us, (flags & lx::kMsgDontwait) != 0, false, true,
                      [&]() -> int64_t {
                        return sendto(s.handle(), reinterpret_cast<const char*>(buf.data()), size, win_msg_flags(flags),
                                      to ? reinterpret_cast<const sockaddr*>(&to->ss) : nullptr, to ? to->len : 0);
                      });
    return rt::ret_int(int32_t(n));
  };
  r.bind(lxc, "sendtoBytes", "(Ljava/io/FileDescriptor;Ljava/lang/Object;IIILjava/net/InetAddress;I)I",
         [send_to](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto s = socket_arg(rt, a.h(1), "sendto");
           if (a.h(6) == kNull) return send_to(rt, *s, a, nullptr);
           Endpoint to = endpoint_for(rt, *s, a.h(6), a.i32(7), "sendto");
           return send_to(rt, *s, a, &to);
         });
  r.bind(lxc, "sendtoBytes", "(Ljava/io/FileDescriptor;Ljava/lang/Object;IIILjava/net/SocketAddress;)I",
         [send_to](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto s = socket_arg(rt, a.h(1), "sendto");
           if (a.h(6) == kNull) return send_to(rt, *s, a, nullptr);
           Endpoint to = socket_address_arg(rt, *s, a.h(6), "sendto");
           return send_to(rt, *s, a, &to);
         });

  // Options.
  r.bind(lxc, "getsockoptInt", "(Ljava/io/FileDescriptor;II)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = socket_arg(rt, a.h(1), "getsockopt");
    return rt::ret_int(get_int_option(rt, *s, a.i32(2), a.i32(3)));
  });
  r.bind(lxc, "setsockoptInt", "(Ljava/io/FileDescriptor;III)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = socket_arg(rt, a.h(1), "setsockopt");
    set_int_option(rt, *s, a.i32(2), a.i32(3), a.i32(4));
    return 0;
  });
  // IP_MULTICAST_TTL and IP_MULTICAST_LOOP as a byte (Linux accepts both sizes).
  r.bind(lxc, "getsockoptByte", "(Ljava/io/FileDescriptor;II)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = socket_arg(rt, a.h(1), "getsockopt");
    return rt::ret_int(get_int_option(rt, *s, a.i32(2), a.i32(3)) & 0xff);
  });
  r.bind(lxc, "setsockoptByte", "(Ljava/io/FileDescriptor;III)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = socket_arg(rt, a.h(1), "setsockopt");
    set_int_option(rt, *s, a.i32(2), a.i32(3), a.i32(4) & 0xff);
    return 0;
  });
  r.bind(lxc, "getsockoptLinger", "(Ljava/io/FileDescriptor;II)Landroid/system/StructLinger;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto s = socket_arg(rt, a.h(1), "getsockopt");
           if (a.i32(2) != lx::kSolSocket || a.i32(3) != lx::kSoLinger) throw_errno(rt, "getsockopt", lx::kEINVAL);
           LINGER l{};
           int len = sizeof l;
           if (getsockopt(s->handle(), SOL_SOCKET, SO_LINGER, reinterpret_cast<char*>(&l), &len) == SOCKET_ERROR) {
             throw_errno(rt, "getsockopt", linux_errno(WSAGetLastError()));
           }
           return construct(rt, "Landroid/system/StructLinger;", "(II)V", {uint32_t(l.l_onoff), uint32_t(l.l_linger)});
         });
  r.bind(lxc, "setsockoptLinger", "(Ljava/io/FileDescriptor;IILandroid/system/StructLinger;)V",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto s = socket_arg(rt, a.h(1), "setsockopt");
           if (a.i32(2) != lx::kSolSocket || a.i32(3) != lx::kSoLinger) throw_errno(rt, "setsockopt", lx::kEINVAL);
           Handle linger = a.h(4);
           if (linger == kNull) rt.throw_npe({});
           LINGER l{};
           l.l_onoff = u_short(field_of(rt, linger, "Landroid/system/StructLinger;", "l_onoff", "I") != 0);
           l.l_linger = u_short(std::clamp<int32_t>(int32_t(field_of(rt, linger, "Landroid/system/StructLinger;", "l_linger", "I")),
                                                    0, 0xffff));
           if (setsockopt(s->handle(), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof l) == SOCKET_ERROR) {
             throw_errno(rt, "setsockopt", linux_errno(WSAGetLastError()));
           }
           return 0;
         });
  // SO_RCVTIMEO and SO_SNDTIMEO, kept for the waits of blocking calls.
  r.bind(lxc, "setsockoptTimeval", "(Ljava/io/FileDescriptor;IILandroid/system/StructTimeval;)V",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto s = socket_arg(rt, a.h(1), "setsockopt");
           int32_t level = a.i32(2), option = a.i32(3);
           int64_t us = timeval_us(rt, a.h(4));
           if (level != lx::kSolSocket || (option != lx::kSoRcvtimeo && option != lx::kSoSndtimeo)) {
             throw_errno(rt, "setsockopt", lx::kEINVAL);
           }
           if (us < 0) throw_errno(rt, "setsockopt", lx::kEDOM);
           (option == lx::kSoRcvtimeo ? s->rcvtimeo_us : s->sndtimeo_us) = us;
           return 0;
         });
  r.bind(lxc, "getsockoptTimeval", "(Ljava/io/FileDescriptor;II)Landroid/system/StructTimeval;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto s = socket_arg(rt, a.h(1), "getsockopt");
           int32_t level = a.i32(2), option = a.i32(3);
           if (level != lx::kSolSocket || (option != lx::kSoRcvtimeo && option != lx::kSoSndtimeo)) {
             rt.throw_new("Ljava/lang/IllegalArgumentException;", "getsockoptTimeval() unsupported with level " +
                                                                       std::to_string(level) + " and option " +
                                                                       std::to_string(option));
           }
           int64_t us = option == lx::kSoRcvtimeo ? s->rcvtimeo_us.load() : s->sndtimeo_us.load();
           int64_t sec = us / 1000000, usec = us % 1000000;
           return construct(rt, "Landroid/system/StructTimeval;", "(JJ)V",
                            {uint32_t(uint64_t(sec)), uint32_t(uint64_t(sec) >> 32), uint32_t(uint64_t(usec)),
                             uint32_t(uint64_t(usec) >> 32)});
         });

  // poll(2): the structs' revents written back, the count of ready
  // descriptors returned; timeoutMs < 0 waits for ever.
  r.bind(lxc, "poll", "([Landroid/system/StructPollfd;I)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    Handle structs = a.h(1);
    if (structs == kNull) rt.throw_npe({});
    int32_t timeout = a.i32(2);
    rt::Class* pollfd = rt.find_class("Landroid/system/StructPollfd;");
    rt::Field* fd_f = find_field(pollfd, "fd", "Ljava/io/FileDescriptor;");
    rt::Field* events_f = find_field(pollfd, "events", "S");
    rt::Field* revents_f = find_field(pollfd, "revents", "S");
    std::vector<PollEntry> entries;
    std::vector<Handle> items;
    uint32_t length = rt.array(structs, "StructPollfd[]").length;
    for (uint32_t i = 0; i < length; ++i) {
      Handle item;
      std::memcpy(&item, &rt.array(structs, "StructPollfd[]").data[size_t(i) * 4], 4);
      if (item == kNull) break;  // trailing nulls allowed
      Handle java_fd = rt.object(item)->fields.at(fd_f->slot);
      if (java_fd == kNull) break;  // Selector clears the fd field
      PollEntry e;
      e.fd = fd_of(rt, java_fd);
      e.events = int16_t(rt.object(item)->fields.at(events_f->slot));
      entries.push_back(std::move(e));
      items.push_back(item);
    }
    // What each descriptor number is now; again after a signalled close (the
    // EINTR that makes Linux_poll start over), which may have closed it or put
    // a marker descriptor under its number.
    auto resolve = [&] {
      for (PollEntry& e : entries) {
        e.sock = nullptr;
        e.revents = 0;
        if (e.fd < 0) continue;
        if (auto s = socket_of(e.fd)) {
          e.sock = std::move(s);
        } else if (e.fd <= 2 || libcore_vfs().is_open(e.fd)) {
          e.revents = int16_t(e.events & (lx::kPollIn | lx::kPollOut | lx::kPollRdnorm | lx::kPollWrnorm));
        } else {
          e.revents = lx::kPollNval;
        }
      }
    };
    std::vector<int32_t> fds;
    for (const PollEntry& e : entries) fds.push_back(e.fd);
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(timeout, 0));
    int ready = 0;
    for (;;) {
      resolve();
      ready = poll_once(entries, 0);
      if (ready > 0 || timeout == 0) break;
      bool signaled = false;
      {
        WaitRegistration registration(fds);
        rt.run_blocking([&] {
          for (;;) {
            if (registration.signaled()) {
              signaled = true;
              return;
            }
            int slice = 50;
            if (timeout > 0) {
              auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
              if (left <= 0) return;
              slice = int(std::min<int64_t>(slice, left));
            }
            bool any_socket = std::any_of(entries.begin(), entries.end(), [](const PollEntry& e) { return e.sock != nullptr; });
            if (!any_socket) {
              std::this_thread::sleep_for(std::chrono::milliseconds(slice));
              continue;
            }
            ready = poll_once(entries, slice);
            if (ready > 0) return;
          }
        });
      }
      if (!signaled) break;
    }
    for (size_t i = 0; i < items.size(); ++i) rt.object(items[i])->fields.at(revents_f->slot) = uint32_t(uint16_t(entries[i].revents));
    return rt::ret_int(ready);
  }, S::kPartial, "sockets par WSAPoll ; les autres descripteurs toujours prêts");

  // Names.
  r.bind(lxc, "android_getaddrinfo", "(Ljava/lang/String;Landroid/system/StructAddrinfo;I)[Ljava/net/InetAddress;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           if (a.h(1) == kNull || a.h(2) == kNull) rt.throw_npe({});
           std::string node = rt.string_utf8(a.h(1));
           const char* ai = "Landroid/system/StructAddrinfo;";
           addrinfo hints{};
           hints.ai_flags = int32_t(field_of(rt, a.h(2), ai, "ai_flags", "I"));
           hints.ai_family = win_family(int32_t(field_of(rt, a.h(2), ai, "ai_family", "I")));
           hints.ai_socktype = int32_t(field_of(rt, a.h(2), ai, "ai_socktype", "I"));
           hints.ai_protocol = int32_t(field_of(rt, a.h(2), ai, "ai_protocol", "I"));
           if (hints.ai_family < 0) throw_gai(rt, "android_getaddrinfo", lx::kEaiFamily, 0);
           // A numeric host (AI_NUMERICHOST, InetAddressUtils' parsing) is parsed
           // in the process: neither the hosts file nor the permission apply.
           constexpr int32_t kAiNumericHost = 4;
           bool numeric = (hints.ai_flags & kAiNumericHost) != 0;
           if (!numeric && !libcore_config().internet) throw_gai(rt, "android_getaddrinfo", lx::kEaiNodata, lx::kEPERM);
           std::vector<sockaddr_storage> found;
           if (!numeric) found = hosts_lookup(node, hints.ai_family);
           if (found.empty()) {
             ensure_winsock();
             addrinfo* list = nullptr;
             int rc = 0;
             rt.run_blocking([&] { rc = getaddrinfo(node.c_str(), nullptr, &hints, &list); });
             if (rc != 0) throw_gai(rt, "android_getaddrinfo", linux_eai(rc), 0);
             for (addrinfo* p = list; p; p = p->ai_next) {
               if (p->ai_family != AF_INET && p->ai_family != AF_INET6) continue;
               sockaddr_storage ss{};
               std::memcpy(&ss, p->ai_addr, std::min<size_t>(p->ai_addrlen, sizeof ss));
               found.push_back(ss);
             }
             freeaddrinfo(list);
           }
           if (found.empty()) return kNull;
           rt::GlobalRef result(rt, rt.new_array("[Ljava/net/InetAddress;", int32_t(found.size())));
           for (size_t i = 0; i < found.size(); ++i) {
             Handle address = to_inet_address(rt, reinterpret_cast<const sockaddr*>(&found[i]), nullptr);
             std::memcpy(&rt.array(result.get(), "InetAddress[]").data[i * 4], &address, 4);
           }
           return result.get();
         }, S::kPartial, "/system/etc/hosts de l'image puis le résolveur de l'hôte ; netId ignoré");
  r.bind(lxc, "getnameinfo", "(Ljava/net/InetAddress;I)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    Endpoint e = to_sockaddr(rt, a.h(1), 0, false);
    int32_t flags = a.i32(2);
    constexpr int32_t kNumericHost = 2;
    if (!(flags & kNumericHost)) {
      if (!libcore_config().internet) throw_gai(rt, "getnameinfo", lx::kEaiNodata, lx::kEPERM);
      if (auto name = hosts_reverse(reinterpret_cast<const sockaddr*>(&e.ss))) return rt.new_string_utf8(*name);
    }
    ensure_winsock();
    char host[NI_MAXHOST] = {};
    int rc = 0;
    rt.run_blocking([&] {
      rc = getnameinfo(reinterpret_cast<const sockaddr*>(&e.ss), e.len, host, sizeof host, nullptr, 0, flags);
    });
    if (rc != 0) throw_gai(rt, "getnameinfo", linux_eai(rc), 0);
    return rt.new_string_utf8(host);
  }, S::kPartial, "/system/etc/hosts de l'image puis le résolveur de l'hôte");
  r.bind(lxc, "gai_strerror", "(I)Ljava/lang/String;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t { return rt.new_string_utf8(gai_text(a.i32(1))); });
  r.bind(lxc, "inet_pton", "(ILjava/lang/String;)Ljava/net/InetAddress;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(2) == kNull) rt.throw_npe({});
    int af = win_family(a.i32(1));
    if (af != AF_INET && af != AF_INET6) return kNull;
    sockaddr_storage ss{};
    void* dst = af == AF_INET ? static_cast<void*>(&reinterpret_cast<sockaddr_in&>(ss).sin_addr)
                              : static_cast<void*>(&reinterpret_cast<sockaddr_in6&>(ss).sin6_addr);
    if (InetPtonA(af, rt.string_utf8(a.h(2)).c_str(), dst) != 1) return kNull;
    ss.ss_family = ADDRESS_FAMILY(af);
    return to_inet_address(rt, reinterpret_cast<const sockaddr*>(&ss), nullptr);
  });

  // socketpair: AF_UNIX only (PlainSocketImpl's marker descriptor, local
  // pipes), as two connected loopback sockets.
  r.bind(lxc, "socketpair", "(IIILjava/io/FileDescriptor;Ljava/io/FileDescriptor;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(4) == kNull) rt.throw_npe("null fd1");
    if (a.h(5) == kNull) rt.throw_npe("null fd2");
    int32_t domain = a.i32(1), type = a.i32(2) & ~(lx::kSockNonblock | lx::kSockCloexec);
    if (domain != lx::kAfUnix) throw_errno(rt, "socketpair", lx::kEOPNOTSUPP);
    if (type != lx::kSockStream && type != lx::kSockDgram) throw_errno(rt, "socketpair", lx::kESOCKTNOSUPPORT);
    ensure_winsock();
    auto [one, two] = loopback_pair(type == lx::kSockStream ? SOCK_STREAM : SOCK_DGRAM);
    if (one == INVALID_SOCKET) throw_errno(rt, "socketpair", linux_errno(WSAGetLastError()));
    auto first = std::make_shared<NetSocket>(one, lx::kAfUnix, type, 0, AF_INET);
    auto second = std::make_shared<NetSocket>(two, lx::kAfUnix, type, 0, AF_INET);
    bool nonblocking = (a.i32(2) & lx::kSockNonblock) != 0;
    first->nonblocking = nonblocking;
    second->nonblocking = nonblocking;
    for (auto [java_fd, sock] : {std::pair{a.h(4), first}, std::pair{a.h(5), second}}) {
      Handle made = install_socket(rt, sock, "socketpair");
      rt::Field* f = find_field(rt.class_of(made), "descriptor", "I");
      field_of(rt, java_fd, "Ljava/io/FileDescriptor;", "descriptor", "I") = rt.object(made)->fields.at(f->slot);
    }
    return 0;
  }, S::kPartial, "AF_UNIX émulé par deux sockets TCP (ou UDP) de la boucle locale");

  // SocketInputStream.socketRead0 and SocketOutputStream.socketWrite0
  // (ojluni's SocketInputStream.c, SocketOutputStream.c): at most 64 KiB per
  // read, the timeout as poll's, the errors as their messages.
  r.bind("Ljava/net/SocketInputStream;", "socketRead0", "(Ljava/io/FileDescriptor;[BIII)I",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto s = stream_socket(rt, a.h(1));
           constexpr int32_t kMaxHeapBufferLen = 65536;
           int32_t len = std::min(a.i32(4), kMaxHeapBufferLen);
           auto buf = extract_io_buffer(rt, a.h(2), a.i32(3), std::max(len, 0), true);
           if (int32_t timeout = a.i32(5); timeout > 0) {
             switch (wait_socket(rt, *s, fd_of(rt, a.h(1)), POLLRDNORM, int64_t(timeout) * 1000)) {
               case Wait::kTimeout: throw_net(rt, "Ljava/net/SocketTimeoutException;", "Read timed out");
               case Wait::kClosed: throw_net(rt, "Ljava/net/SocketException;", "Socket closed");
               case Wait::kReady: break;
             }
           }
           auto [n, err] = stream_io(rt, *s, fd_of(rt, a.h(1)), POLLRDNORM, false, [&]() -> int64_t {
             return recv(s->handle(), reinterpret_cast<char*>(buf.data()), int(buf.size()), 0);
           });
           if (n >= 0) return rt::ret_int(int32_t(n));
           switch (err) {
             case lx::kECONNRESET:
             case lx::kEPIPE: throw_net(rt, "Lsun/net/ConnectionResetException;", "Connection reset");
             case lx::kEBADF: throw_net(rt, "Ljava/net/SocketException;", "Socket closed");
             case lx::kEINTR: throw_net(rt, "Ljava/io/InterruptedIOException;", "Operation interrupted");
             default: throw_net(rt, "Ljava/net/SocketException;", strerror_text(err));
           }
         });
  r.bind("Ljava/net/SocketOutputStream;", "socketWrite0", "(Ljava/io/FileDescriptor;[BII)V",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto s = stream_socket(rt, a.h(1));
           auto buf = extract_io_buffer(rt, a.h(2), a.i32(3), std::max(a.i32(4), 0), false);
           size_t done = 0;
           while (done < buf.size()) {
             int chunk = int(std::min<size_t>(buf.size() - done, 65536));
             auto [n, err] = stream_io(rt, *s, fd_of(rt, a.h(1)), POLLWRNORM, true, [&]() -> int64_t {
               return send(s->handle(), reinterpret_cast<const char*>(buf.data() + done), chunk, 0);
             });
             if (n < 0) {
               if (err == lx::kECONNRESET) throw_net(rt, "Lsun/net/ConnectionResetException;", "Connection reset");
               throw_net(rt, "Ljava/net/SocketException;", strerror_text(err));
             }
             done += size_t(n);
           }
           return 0;
         });
}

}  // namespace aa::bridge
