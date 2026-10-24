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
#include <iphlpapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
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
#include "framework-bridge/native_memory.h"
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
constexpr int32_t kSockStream = 1, kSockDgram = 2, kSockRaw = 3, kSockSeqpacket = 5, kSockNonblock = 04000,
                  kSockCloexec = 02000000;
constexpr int32_t kSolSocket = 1, kIpprotoIp = 0, kIpprotoTcp = 6, kIpprotoIpv6 = 41;
constexpr int32_t kSoReuseaddr = 2, kSoType = 3, kSoError = 4, kSoDontroute = 5, kSoBroadcast = 6, kSoSndbuf = 7,
                  kSoRcvbuf = 8, kSoKeepalive = 9, kSoOobinline = 10, kSoLinger = 13, kSoReuseport = 15,
                  kSoPasscred = 16, kSoRcvlowat = 18, kSoSndlowat = 19, kSoRcvtimeo = 20, kSoSndtimeo = 21, kSoAcceptconn = 30,
                  kSoProtocol = 38, kSoDomain = 39;
constexpr int32_t kTcpNodelay = 1, kTcpMaxseg = 2, kTcpKeepidle = 4, kTcpKeepintvl = 5, kTcpKeepcnt = 6;
constexpr int32_t kIpTos = 1, kIpTtl = 2, kIpMulticastTtl = 33, kIpMulticastLoop = 34, kIpMulticastAll = 49;
constexpr int32_t kIpv6UnicastHops = 16, kIpv6MulticastHops = 18, kIpv6MulticastLoop = 19, kIpv6V6only = 26,
                  kIpv6Tclass = 67;
constexpr int16_t kPollIn = 0x1, kPollOut = 0x4, kPollErr = 0x8, kPollHup = 0x10, kPollNval = 0x20,
                  kPollRdnorm = 0x40, kPollWrnorm = 0x100;
constexpr int32_t kMsgOob = 1, kMsgPeek = 2, kMsgDontroute = 4, kMsgDontwait = 0x40;
enum : int32_t {
  kEPERM = 1, kENOENT = 2, kEINTR = 4, kEIO = 5, kEBADF = 9, kEAGAIN = 11, kENOMEM = 12, kEACCES = 13, kEFAULT = 14,
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

// ipv6_available() of net_util_md.c: whether an IPv6 socket can be made.
bool ipv6_available() {
  static const bool available = [] {
    ensure_winsock();
    SOCKET s = socket(AF_INET6, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return false;
    closesocket(s);
    return true;
  }();
  return available;
}

// A socket of the emulated process. Reads and writes reaching it through the
// Vfs directly (other paths than libcore's descriptor natives) never block.
// A pipe (pipe2, IOUtil.makePipe) is one too: Windows has no pipe that WSAPoll
// waits on with sockets, so its ends are a pair of loopback sockets, as in
// OpenJDK's Windows PipeImpl.
class NetSocket final : public vfs::OpenFile {
 public:
  enum class Pipe { kNone, kRead, kWrite };
  NetSocket(SOCKET handle, int32_t family_, int32_t type_, int32_t protocol_, int win_af_, Pipe pipe_ = Pipe::kNone)
      : family(family_), type(type_), protocol(protocol_), win_af(win_af_), pipe(pipe_), ino(next_ino()),
        handle_(handle) {}
  ~NetSocket() override { closesocket(handle_); }
  NetSocket(const NetSocket&) = delete;
  NetSocket& operator=(const NetSocket&) = delete;

  vfs::VStat stat() override {
    vfs::VStat st;
    st.type = pipe == Pipe::kNone ? vfs::NodeType::kSocket : vfs::NodeType::kFifo;
    st.mode = pipe == Pipe::kNone ? 0140777 : 010600;  // S_IFSOCK, S_IFIFO, as the kernel reports them
    st.ino = ino;
    return st;
  }
  size_t read(uint64_t, std::span<uint8_t> out) override;  // after the AF_UNIX helpers
  size_t write(uint64_t, std::span<const uint8_t> data) override;
  void truncate(uint64_t) override { throw vfs::VfsError{lx::kEINVAL, "ftruncate"}; }
  bool seekable() const override { return false; }

  SOCKET handle() const { return handle_; }

  const int32_t family, type, protocol;  // Linux values
  // The Winsock family: AF_INET for an AF_INET6 socket of a host without
  // IPv6, which then takes IPv4 and IPv4-mapped addresses only.
  const int win_af;
  const Pipe pipe;
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
  // AF_UNIX (see "AF_UNIX" below): the bound name, sun_path as getsockname
  // reports it (empty: unnamed), the connected peer's, listen() called,
  // SO_PASSCRED (kept, no credentials are sent).
  std::string unix_name, unix_peer;
  bool unix_connected = false, unix_listening = false;
  int32_t passcred = 0;

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
  if (!s || s->pipe != NetSocket::Pipe::kNone) throw_errno(rt, function, lx::kENOTSOCK);
  return s;
}

// The Linux errno of a failed I/O on `s`: a pipe whose other end is closed
// fails writes with EPIPE (its loopback connection is reset or shut down).
int32_t io_errno(const NetSocket& s, int wsa, bool sending) {
  if (s.pipe != NetSocket::Pipe::kNone && sending &&
      (wsa == WSAECONNRESET || wsa == WSAECONNABORTED || wsa == WSAESHUTDOWN)) {
    return lx::kEPIPE;
  }
  return linux_errno(wsa, sending);
}

// read(2) on a pipe's write end, write(2) on its read end: EBADF.
bool wrong_pipe_end(const NetSocket& s, bool sending) {
  return s.pipe == (sending ? NetSocket::Pipe::kRead : NetSocket::Pipe::kWrite);
}

[[noreturn]] void throw_net(Runtime& rt, const char* cls, const std::string& message) {
  throw rt::JavaThrow{construct(rt, cls, "(Ljava/lang/String;)V", {rt.new_string_utf8(message)})};
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

enum class Wait { kReady, kTimeout, kClosed, kInterrupted };

// AsynchronousCloseMonitor: the waits blocked on descriptor numbers, which
// signalBlockedThreads(fd) interrupts. By number, as in libcore: a close
// (closeAndSignalBlockedThreads) or PlainSocketImpl's dup2 of its marker
// descriptor over the socket's number wakes whoever waits on that number.
// NIO's waits also name their thread: NativeThread.signal(thread), the
// signal that makes Linux's blocking call fail with EINTR, interrupts them.
struct BlockedWait {
  std::vector<int32_t> fds;
  uint32_t thread = 0;  // VmThread id for NIO's waits, else 0
  std::atomic<Wait> signal{Wait::kReady};  // kClosed or kInterrupted once signalled
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
  explicit WaitRegistration(std::vector<int32_t> fds, uint32_t thread = 0) {
    wait_.fds = std::move(fds);
    wait_.thread = thread;
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
  bool signaled() const { return wait_.signal != Wait::kReady; }
  Wait signal() const { return wait_.signal; }

 private:
  BlockedWait wait_;
};

// The id NativeThread.current() gives the running thread (never 0).
uint32_t current_thread_id(Runtime& rt) {
  rt::VmThread* t = rt.current_thread();
  return t ? t->id : 0;
}

// The socket's pending error (SO_ERROR), as Winsock reports it.
int pending_error(SOCKET s) {
  int err = 0;
  int len = sizeof err;
  if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) == SOCKET_ERROR) return WSAGetLastError();
  return err;
}

// Waits, the GIL released, until the socket is ready for `events`
// (POLLRDNORM, POLLWRNORM), fails or hangs up, a close is signalled from
// another thread, or the timeout (microseconds; 0: none) expires; an
// `interruptible` wait (NIO's) also ends on NativeThread.signal. A failed
// connect is also seen through SO_ERROR, which WSAPoll does not report on
// every Windows version.
Wait wait_socket(Runtime& rt, NetSocket& s, int32_t fd, short events, int64_t timeout_us, bool interruptible = false) {
  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + std::chrono::microseconds(timeout_us);
  Wait result = Wait::kReady;
  WaitRegistration registration({fd}, interruptible ? current_thread_id(rt) : 0);
  rt.run_blocking([&] {
    for (;;) {
      if (registration.signaled()) {
        result = registration.signal();
        return;
      }
      if (rt.shutting_down()) return;
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
    if (err != WSAEWOULDBLOCK || s.nonblocking || dontwait) throw_errno(rt, function, io_errno(s, err, sending));
    switch (wait_socket(rt, s, fd, events, timeout_us)) {
      case Wait::kClosed: throw_closed(rt, io, function);
      case Wait::kTimeout: throw_errno(rt, function, lx::kEAGAIN);
      case Wait::kReady:
      case Wait::kInterrupted: break;
    }
  }
}

// IO_FAILURE_RETRY_NOTHROW: the call's result, or -1 with the Linux errno
// in `err` (EBADF once a close is signalled, EAGAIN after a timeout).
template <typename Call>
int64_t retry_nothrow(Runtime& rt, NetSocket& s, int32_t fd, short events, int64_t timeout_us, bool dontwait,
                      bool sending, int32_t& err, Call call) {
  for (;;) {
    int64_t r = call();
    if (r >= 0) return r;
    int wsa = WSAGetLastError();
    if (wsa == WSAEINTR) continue;
    if (wsa != WSAEWOULDBLOCK || s.nonblocking || dontwait) {
      err = io_errno(s, wsa, sending);
      return -1;
    }
    switch (wait_socket(rt, s, fd, events, timeout_us)) {
      case Wait::kClosed: err = lx::kEBADF; return -1;
      case Wait::kTimeout: err = lx::kEAGAIN; return -1;
      case Wait::kReady:
      case Wait::kInterrupted: break;
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
// An IPv6 endpoint for a Winsock AF_INET socket: the IPv4 one of a mapped or
// unspecified address; false for another IPv6 address.
bool to_ipv4(const Endpoint& e, Endpoint& v4) {
  const auto& sin6 = reinterpret_cast<const sockaddr_in6&>(e.ss);
  bool unspecified = IN6_IS_ADDR_UNSPECIFIED(&sin6.sin6_addr), mapped = IN6_IS_ADDR_V4MAPPED(&sin6.sin6_addr);
  if (!unspecified && !mapped) return false;
  v4 = Endpoint{};
  auto& sin = reinterpret_cast<sockaddr_in&>(v4.ss);
  sin.sin_family = AF_INET;
  sin.sin_port = sin6.sin6_port;
  if (mapped) std::memcpy(&sin.sin_addr, &sin6.sin6_addr.s6_addr[12], 4);
  v4.len = sizeof(sockaddr_in);
  return true;
}

Endpoint endpoint_for(Runtime& rt, NetSocket& s, Handle inet, int32_t port, const char* function) {
  Endpoint e = to_sockaddr(rt, inet, port, s.family == lx::kAfInet6 && s.win_af == AF_INET6);
  if (s.win_af != AF_INET || e.ss.ss_family != AF_INET6) return e;
  Endpoint v4;
  if (!to_ipv4(e, v4)) {
    throw_errno(rt, function, std::string_view(function) == "bind" ? lx::kEADDRNOTAVAIL : lx::kENETUNREACH);
  }
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

// getsockopt(2) of an int option into `value`: 0, else the Linux errno.
int32_t int_option(NetSocket& s, int32_t level, int32_t option, int32_t& value) {
  if (level == lx::kSolSocket) {
    switch (option) {
      case lx::kSoType: value = s.type; return 0;
      case lx::kSoDomain: value = s.family; return 0;
      case lx::kSoProtocol:
        value = s.family == lx::kAfUnix ? 0 : s.protocol != 0 ? s.protocol : s.type == lx::kSockStream ? IPPROTO_TCP : IPPROTO_UDP;
        return 0;
      case lx::kSoPasscred: value = s.passcred; return 0;
      case lx::kSoError: {
        int err = pending_error(s.handle());
        s.connecting = false;
        value = err == 0 ? 0 : linux_errno(err);
        return 0;
      }
      case lx::kSoReuseaddr:
        if (s.type != lx::kSockStream) break;
        value = s.reuseaddr;
        return 0;
      case lx::kSoReuseport: value = 0; return 0;
      default: break;
    }
  }
  if (level == lx::kIpprotoIp && option == lx::kIpMulticastAll) {
    value = s.multicast_all;
    return 0;
  }
  auto w = win_option(level, option);
  if (!w) return lx::kENOPROTOOPT;
  DWORD v = 0;
  int len = sizeof v;
  if (getsockopt(s.handle(), w->level, w->name, reinterpret_cast<char*>(&v), &len) == SOCKET_ERROR) {
    return linux_errno(WSAGetLastError());
  }
  if (len == 1) v &= 0xff;  // BOOLEAN-sized options
  value = int32_t(v);
  return 0;
}

// setsockopt(2) of an int option: 0, else the Linux errno.
int32_t set_option(NetSocket& s, int32_t level, int32_t option, int32_t value) {
  if (level == lx::kSolSocket && option == lx::kSoReuseaddr && s.type == lx::kSockStream) {
    s.reuseaddr = value != 0;  // Linux's rebinding after TIME_WAIT is Windows' default
    return 0;
  }
  if (level == lx::kSolSocket && option == lx::kSoReuseport) return lx::kENOPROTOOPT;
  if (level == lx::kSolSocket && option == lx::kSoPasscred) {  // kept; no SCM_CREDENTIALS are sent
    s.passcred = value != 0;
    return 0;
  }
  if (level == lx::kIpprotoIp && option == lx::kIpMulticastAll) {
    s.multicast_all = value != 0;
    return 0;
  }
  auto w = win_option(level, option);
  if (!w) return lx::kENOPROTOOPT;
  DWORD v = DWORD(value);
  if (setsockopt(s.handle(), w->level, w->name, reinterpret_cast<const char*>(&v), sizeof v) == SOCKET_ERROR) {
    return linux_errno(WSAGetLastError());
  }
  return 0;
}

int32_t get_int_option(Runtime& rt, NetSocket& s, int32_t level, int32_t option) {
  int32_t value = 0;
  if (int32_t err = int_option(s, level, option, value)) throw_errno(rt, "getsockopt", err);
  return value;
}

void set_int_option(Runtime& rt, NetSocket& s, int32_t level, int32_t option, int32_t value) {
  if (int32_t err = set_option(s, level, option, value)) throw_errno(rt, "setsockopt", err);
}

// --- Messages (recvmsg, sendmsg) --------------------------------------------------------

constexpr int32_t kAppUid = 10000;  // the application's uid (u0_a0), as libcore_natives.cpp reports it
constexpr std::string_view kStructMsghdr = "Landroid/system/StructMsghdr;";

// The buffers of StructMsghdr.msg_iov as libcore's byteBufferArrayToIOV takes
// them: from the start of each buffer (a direct buffer's address, a heap
// buffer's array), remaining() bytes long, its position and array offset
// aside.
std::vector<WSABUF> message_buffers(Runtime& rt, Handle msg, bool writable) {
  if (msg == kNull) rt.throw_npe("missing structMsghdr");
  Handle iov = field_of(rt, msg, kStructMsghdr, "msg_iov", "[Ljava/nio/ByteBuffer;");
  if (iov == kNull) rt.throw_npe("null StructMsghdr.msg_iov");
  uint32_t count = rt.array(iov, "ByteBuffer[]").length;
  std::vector<WSABUF> out;
  for (uint32_t i = 0; i < count; ++i) {
    Handle b;
    std::memcpy(&b, &rt.array(iov, "ByteBuffer[]").data[size_t(i) * 4], 4);
    if (b == kNull) rt.throw_npe({});
    int32_t remaining = int32_t(rt.call_virtual(b, "remaining", "()I"));
    std::span<uint8_t> bytes;
    if (rt.call_virtual(b, "isDirect", "()Z") != 0) {
      rt::Field* f = find_field(rt.find_class("Ljava/nio/Buffer;"), "address", "J");
      rt::Object& o = rt.nonnull(b, "ByteBuffer");
      int64_t address = int64_t(uint64_t(o.fields.at(f->slot)) | (uint64_t(o.fields.at(f->slot + 1)) << 32));
      if (remaining > 0) bytes = native_span(rt, address, uint64_t(remaining), writable);
    } else {
      rt::ArrayObject& array = rt.array(Handle(rt.call_virtual(b, "array", "()[B")), "byte[]");
      if (uint32_t(remaining) > array.length) rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", "msg_iov");
      bytes = std::span<uint8_t>(array.data.data(), size_t(remaining));
    }
    WSABUF w;
    w.len = ULONG(bytes.size());
    w.buf = reinterpret_cast<char*>(bytes.data());
    out.push_back(w);
  }
  return out;
}

// --- Network interfaces -----------------------------------------------------------------

// Linux interface flags (if.h).
constexpr uint32_t kIffUp = 0x1, kIffBroadcast = 0x2, kIffLoopback = 0x8, kIffRunning = 0x40, kIffMulticast = 0x1000;

struct IfAddress {
  sockaddr_storage addr{}, netmask{}, broadcast{};
  bool has_broadcast = false;
};

struct Interface {
  std::string name;
  int32_t index = 0;
  uint32_t flags = 0;
  int32_t mtu = 0;
  ULONG win_index = 0;  // the Windows interface index (0 for lo)
  std::vector<IfAddress> addresses;
};

void ipv4_mask(sockaddr_storage& ss, unsigned prefix) {
  auto& sin = reinterpret_cast<sockaddr_in&>(ss);
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = htonl(prefix == 0 ? 0 : uint32_t(0xffffffffu << (32 - std::min(prefix, 32u))));
}

void ipv6_mask(sockaddr_storage& ss, unsigned prefix) {
  auto& sin6 = reinterpret_cast<sockaddr_in6&>(ss);
  sin6.sin6_family = AF_INET6;
  for (unsigned i = 0; i < 16; ++i) {
    unsigned bits = prefix > i * 8 ? std::min(8u, prefix - i * 8) : 0;
    sin6.sin6_addr.s6_addr[i] = uint8_t(bits == 0 ? 0 : 0xff << (8 - bits));
  }
}

// The interfaces an application sees, as bionic's getifaddrs gives them to
// an application targeting Android 11 or later (no RTM_GETLINK, hence no
// link-layer entries and no hardware address): lo, then the host's adapters
// that are up with an address, Wi-Fi as wlanN and Ethernet as ethN, indexed
// from 1 in that order as the kernel numbers them. Link-local IPv6
// addresses carry their interface's index as scope.
std::vector<Interface> interfaces() {
  std::vector<Interface> out;
  Interface lo;
  lo.name = "lo";
  lo.index = 1;
  lo.flags = kIffUp | kIffLoopback | kIffRunning;
  lo.mtu = 65536;
  IfAddress v4;
  auto& sin = reinterpret_cast<sockaddr_in&>(v4.addr);
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ipv4_mask(v4.netmask, 8);
  lo.addresses.push_back(v4);
  IfAddress v6;
  auto& sin6 = reinterpret_cast<sockaddr_in6&>(v6.addr);
  sin6.sin6_family = AF_INET6;
  sin6.sin6_addr.s6_addr[15] = 1;
  ipv6_mask(v6.netmask, 128);
  lo.addresses.push_back(v6);
  out.push_back(lo);

  ensure_winsock();
  ULONG size = 16 * 1024;
  std::vector<uint8_t> buffer;
  ULONG rc = ERROR_BUFFER_OVERFLOW;
  for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; ++attempt) {
    buffer.assign(size, 0);
    rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                              nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
  }
  if (rc != NO_ERROR) return out;
  int wlan = 0, eth = 0;
  for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp || !a->FirstUnicastAddress) continue;
    bool wifi = a->IfType == IF_TYPE_IEEE80211;
    if (!wifi && a->IfType != IF_TYPE_ETHERNET_CSMACD) continue;  // loopback, tunnels and the like
    Interface itf;
    itf.name = wifi ? "wlan" + std::to_string(wlan++) : "eth" + std::to_string(eth++);
    itf.index = int32_t(out.size()) + 1;
    itf.flags = kIffUp | kIffBroadcast | kIffRunning | kIffMulticast;
    itf.mtu = int32_t(a->Mtu);
    itf.win_index = a->IfIndex ? a->IfIndex : a->Ipv6IfIndex;
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      const sockaddr* sa = u->Address.lpSockaddr;
      if (!sa || (sa->sa_family != AF_INET && sa->sa_family != AF_INET6)) continue;
      if (sa->sa_family == AF_INET6 && !ipv6_available()) continue;
      IfAddress ia;
      std::memcpy(&ia.addr, sa, std::min<size_t>(size_t(u->Address.iSockaddrLength), sizeof ia.addr));
      unsigned prefix = u->OnLinkPrefixLength;
      if (sa->sa_family == AF_INET) {
        ipv4_mask(ia.netmask, prefix);
        if (prefix < 32) {
          auto& b = reinterpret_cast<sockaddr_in&>(ia.broadcast);
          b.sin_family = AF_INET;
          b.sin_addr.s_addr = reinterpret_cast<sockaddr_in&>(ia.addr).sin_addr.s_addr |
                              ~reinterpret_cast<sockaddr_in&>(ia.netmask).sin_addr.s_addr;
          ia.has_broadcast = true;
        }
      } else {
        ipv6_mask(ia.netmask, prefix);
        auto& a6 = reinterpret_cast<sockaddr_in6&>(ia.addr);
        a6.sin6_port = 0;
        a6.sin6_flowinfo = 0;
        a6.sin6_scope_id = IN6_IS_ADDR_LINKLOCAL(&a6.sin6_addr) ? ULONG(itf.index) : 0;
      }
      itf.addresses.push_back(ia);
    }
    if (!itf.addresses.empty()) out.push_back(std::move(itf));
  }
  return out;
}

std::optional<Interface> interface_named(const std::string& name) {
  for (auto& itf : interfaces()) {
    if (itf.name == name) return itf;
  }
  return std::nullopt;
}

// The Windows interface index of a Linux one (0: the default interface).
ULONG win_interface_index(int32_t index) {
  if (index <= 0) return 0;
  for (const auto& itf : interfaces()) {
    if (itf.index == index) return itf.win_index;
  }
  return ULONG(-1);  // unknown: the system call fails as for a missing device
}

// fillIfreq: ifr_name, truncated to IFNAMSIZ - 1 bytes.
std::string ifreq_name(Runtime& rt, Handle name) {
  if (name == kNull) rt.throw_npe({});
  std::string n = rt.string_utf8(name);
  if (n.size() > 15) n.resize(15);
  return n;
}

// --- Natives --------------------------------------------------------------------------------

// A descriptor for the socket (or pipe end), as /proc/self/fd names them;
// the Linux errno (EMFILE) on failure.
int32_t install_fd(std::shared_ptr<NetSocket> sock, int32_t& fd) {
  using P = NetSocket::Pipe;
  int32_t access = sock->pipe == P::kRead ? vfs::kO_RDONLY : sock->pipe == P::kWrite ? vfs::kO_WRONLY : vfs::kO_RDWR;
  int32_t flags = access | (sock->nonblocking ? vfs::kO_NONBLOCK : 0);
  std::string path = std::string(sock->pipe == P::kNone ? "socket:[" : "pipe:[") + std::to_string(sock->ino) + "]";
  try {
    fd = libcore_vfs().install(std::move(sock), flags, std::move(path));
    return 0;
  } catch (const vfs::VfsError& e) {
    return e.err;
  }
}

Handle install_socket(Runtime& rt, std::shared_ptr<NetSocket> sock, const char* function) {
  int32_t fd = -1;
  if (int32_t err = install_fd(std::move(sock), fd)) throw_errno(rt, function, err);
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

Handle new_unix_address(Runtime& rt, const std::string& sun_path);  // AF_UNIX, below

Handle sock_name(Runtime& rt, Handle java_fd, bool peer) {
  const char* function = peer ? "getpeername" : "getsockname";
  auto s = socket_arg(rt, java_fd, function);
  if (s->family == lx::kAfUnix) {
    if (peer && !s->unix_connected) throw_errno(rt, function, lx::kENOTCONN);
    return new_unix_address(rt, peer ? s->unix_peer : s->unix_name);
  }
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

// What each descriptor number is now: a socket polled by WSAPoll, another
// open descriptor ready at once, a closed one POLLNVAL, a negative one ignored.
void resolve_poll(std::vector<PollEntry>& entries) {
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
  // The connection taken must be the client's: another program of the host
  // may connect to the listener meanwhile; its connections are closed.
  sockaddr_in mine{};
  int mine_len = sizeof mine;
  getsockname(client, reinterpret_cast<sockaddr*>(&mine), &mine_len);
  SOCKET server = INVALID_SOCKET;
  for (int tries = 0; tries < 16 && server == INVALID_SOCKET; ++tries) {
    WSAPOLLFD p{};
    p.fd = listener;
    p.events = POLLRDNORM;
    if (WSAPoll(&p, 1, 5000) != 1) return fail(listener, client, INVALID_SOCKET);
    sockaddr_in from{};
    int from_len = sizeof from;
    server = accept(listener, reinterpret_cast<sockaddr*>(&from), &from_len);
    if (server == INVALID_SOCKET) return fail(listener, client, INVALID_SOCKET);
    if (from.sin_port != mine.sin_port || from.sin_addr.s_addr != mine.sin_addr.s_addr) {
      closesocket(server);
      server = INVALID_SOCKET;
    }
  }
  if (server == INVALID_SOCKET) return fail(listener, client, INVALID_SOCKET);
  closesocket(listener);
  u_long on = 1;
  ioctlsocket(server, FIONBIO, &on);
  WSAPOLLFD p{};
  p.fd = client;
  p.events = POLLWRNORM;
  WSAPoll(&p, 1, 5000);  // the connection completes
  return {server, client};
}

// --- AF_UNIX ----------------------------------------------------------------------------
//
// Windows' own AF_UNIX has neither datagrams, SOCK_SEQPACKET nor the abstract
// namespace, and its socket files are reparse points, which host areas refuse.
// An AF_UNIX socket of the process is therefore a loopback socket, TCP for
// SOCK_STREAM and SOCK_SEQPACKET, UDP for SOCK_DGRAM, and its names live here:
//  - an abstract name ("\0" then bytes) or an autobind one, in a table of the
//    process, free again once the socket is closed;
//  - a path names a socket node of the Vfs (S_IFSOCK) whose data identifies
//    the binding: a rename keeps it, and a node left by a closed socket or an
//    earlier run refuses connections (ECONNREFUSED), as on Linux.
// Only the process's own sockets reach them: a connection or a datagram from
// a loopback port that is none of its AF_UNIX sockets (another program of the
// host) is dropped unseen. A SOCK_SEQPACKET record travels as a 4-byte length
// then its bytes and is only taken whole.

constexpr size_t kSunPathMax = 108;  // sizeof(sockaddr_un::sun_path)
constexpr int kUnixBuffer = 212992;  // net.core.rmem_default and wmem_default
constexpr size_t kSeqHeader = 4;
constexpr std::string_view kUnixSocketAddress = "Landroid/system/UnixSocketAddress;";

struct UnixTable {
  std::mutex lock;
  std::map<std::string, std::weak_ptr<NetSocket>> names;  // an abstract name, or "node:" + a socket node's data
  std::map<uint32_t, std::weak_ptr<NetSocket>> ports;     // the loopback port of each (| 0x10000: UDP)
};

UnixTable& unix_table() {
  static UnixTable table;
  return table;
}

uint32_t port_key(int32_t type, uint16_t port) { return (type == lx::kSockDgram ? 0x10000u : 0u) | port; }

template <typename Map>
void forget_closed(Map& map) {
  for (auto it = map.begin(); it != map.end();) it = it->second.expired() ? map.erase(it) : std::next(it);
}

// The loopback address a Winsock socket is bound to (port 0 when unbound).
sockaddr_in loopback_name(SOCKET h) {
  sockaddr_in sin{};
  int len = sizeof sin;
  if (getsockname(h, reinterpret_cast<sockaddr*>(&sin), &len) == SOCKET_ERROR) sin = sockaddr_in{};
  return sin;
}

// Gives AF_UNIX socket `s` its loopback port if it has none, and records it:
// 0, else the Linux errno.
int32_t unix_attach(const std::shared_ptr<NetSocket>& s) {
  sockaddr_in sin = loopback_name(s->handle());
  if (sin.sin_port == 0) {
    sockaddr_in any{};
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s->handle(), reinterpret_cast<sockaddr*>(&any), sizeof any) == SOCKET_ERROR) {
      return linux_errno(WSAGetLastError());
    }
    sin = loopback_name(s->handle());
  }
  UnixTable& t = unix_table();
  std::lock_guard<std::mutex> guard(t.lock);
  if (t.ports.size() >= 1024) forget_closed(t.ports);
  t.ports[port_key(s->type, ntohs(sin.sin_port))] = s;
  return 0;
}

// The AF_UNIX socket of the process behind a loopback source address, if any.
std::shared_ptr<NetSocket> unix_by_address(int32_t type, const sockaddr_storage& ss) {
  if (ss.ss_family != AF_INET) return nullptr;
  const auto& sin = reinterpret_cast<const sockaddr_in&>(ss);
  if (sin.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) return nullptr;
  UnixTable& t = unix_table();
  std::lock_guard<std::mutex> guard(t.lock);
  auto it = t.ports.find(port_key(type, ntohs(sin.sin_port)));
  std::shared_ptr<NetSocket> sock = it == t.ports.end() ? nullptr : it->second.lock();
  return sock && sock->family == lx::kAfUnix && sock->type == type ? sock : nullptr;
}

bool is_unix_socket_address(Runtime& rt, Handle h) { return h != kNull && rt.instance_of(h, rt.find_class(kUnixSocketAddress)); }

// javaUnixSocketAddressToSockaddr: the sun_path bytes, at most 108.
std::string unix_sun_path(Runtime& rt, Handle usa) {
  Handle bytes = field_of(rt, usa, kUnixSocketAddress, "sun_path", "[B");
  if (bytes == kNull) rt.throw_npe({});
  rt::ArrayObject& a = rt.array(bytes, "byte[]");
  if (a.length > kSunPathMax) {
    rt.throw_new("Ljava/lang/IllegalArgumentException;",
                 "sun_path too long: max=" + std::to_string(kSunPathMax) + ", is=" + std::to_string(a.length));
  }
  return std::string(reinterpret_cast<const char*>(a.data.data()), a.length);
}

Handle unix_bytes(Runtime& rt, const std::string& sun_path) {
  Handle bytes = rt.new_array("[B", int32_t(sun_path.size()));
  if (!sun_path.empty()) std::memcpy(rt.array(bytes, "byte[]").data.data(), sun_path.data(), sun_path.size());
  return bytes;
}

// makeSocketAddress of an AF_UNIX address: a UnixSocketAddress.
Handle new_unix_address(Runtime& rt, const std::string& sun_path) {
  rt::GlobalRef bytes(rt, unix_bytes(rt, sun_path));
  return construct(rt, kUnixSocketAddress, "([B)V", {bytes.get()});
}

// fillUnixSocketAddress.
void fill_unix_address(Runtime& rt, Handle usa, const std::string& sun_path) {
  if (usa == kNull) return;
  Handle bytes = unix_bytes(rt, sun_path);
  field_of(rt, usa, kUnixSocketAddress, "sun_path", "[B") = bytes;
}

// The identity of a new socket node: unique in this run and from one run to another.
std::string unix_node_id() {
  static const uint64_t start = [] {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return uint64_t(t.QuadPart);
  }();
  static std::atomic<uint64_t> counter{0};
  char id[64];
  std::snprintf(id, sizeof id, "%lx-%llx-%llx", GetCurrentProcessId(), static_cast<unsigned long long>(start),
                static_cast<unsigned long long>(++counter));
  return id;
}

// bind(2) of an AF_UNIX socket (unix_bind): 0, else the Linux errno. An
// empty sun_path autobinds: an abstract name of five hex digits. A path is
// kept as Linux keeps it (unix_mkname_bsd): up to its first NUL, NUL-terminated.
int32_t unix_bind(const std::shared_ptr<NetSocket>& s, const std::string& sun_path) {
  if (!s->unix_name.empty()) return lx::kEINVAL;  // bound already
  UnixTable& t = unix_table();
  std::string name, path;
  if (sun_path.empty()) {
    static uint32_t next = 0;
    std::lock_guard<std::mutex> guard(t.lock);
    for (uint32_t tries = 0;; ++tries) {
      if (tries > 0xFFFFF) return 28;  // ENOSPC
      char hex[8];
      std::snprintf(hex, sizeof hex, "%05x", unsigned(next++ & 0xFFFFF));
      name = std::string(1, '\0') + hex;
      auto it = t.names.find(name);
      if (it == t.names.end() || it->second.expired()) break;
    }
  } else if (sun_path[0] == '\0') {
    name = sun_path;
    std::lock_guard<std::mutex> guard(t.lock);
    auto it = t.names.find(name);
    if (it != t.names.end() && !it->second.expired()) return lx::kEADDRINUSE;
  } else {
    path = sun_path.substr(0, sun_path.find('\0'));
    name = path + '\0';
  }
  if (int32_t err = unix_attach(s)) return err;
  std::string key = name;
  if (!path.empty()) {
    std::string id = unix_node_id();
    try {
      libcore_vfs().mknod(path, vfs::NodeType::kSocket, id);
    } catch (const vfs::VfsError& e) {
      return e.err == vfs::kEEXIST ? lx::kEADDRINUSE : e.err;
    }
    key = "node:" + id;
  }
  std::lock_guard<std::mutex> guard(t.lock);
  if (t.names.size() >= 1024) forget_closed(t.names);
  t.names[key] = s;
  s->unix_name = name;
  return 0;
}

// The AF_UNIX socket a sun_path names (unix_find_other), else nullptr and the
// Linux errno: the path's own (ENOENT...), ECONNREFUSED when nothing is bound
// to the name or the path is not a socket.
std::shared_ptr<NetSocket> unix_lookup(const std::string& sun_path, int32_t& err) {
  if (sun_path.empty()) {
    err = lx::kEINVAL;
    return nullptr;
  }
  std::string key = sun_path;
  if (sun_path[0] != '\0') {
    try {
      auto [type, data] = libcore_vfs().node(sun_path.substr(0, sun_path.find('\0')));
      if (type != vfs::NodeType::kSocket) {
        err = lx::kECONNREFUSED;
        return nullptr;
      }
      key = "node:" + data;
    } catch (const vfs::VfsError& e) {
      err = e.err;
      return nullptr;
    }
  }
  UnixTable& t = unix_table();
  std::lock_guard<std::mutex> guard(t.lock);
  auto it = t.names.find(key);
  std::shared_ptr<NetSocket> sock = it == t.names.end() ? nullptr : it->second.lock();
  if (!sock) err = lx::kECONNREFUSED;
  return sock;
}

Endpoint unix_endpoint(const NetSocket& target) {
  Endpoint e;
  sockaddr_in sin = loopback_name(target.handle());
  std::memcpy(&e.ss, &sin, sizeof sin);
  e.len = sizeof sin;
  return e;
}

// The loopback socket behind a new AF_UNIX socket: Linux's buffer sizes.
SOCKET new_unix_socket(int32_t type) {
  SOCKET h = new_win_socket(AF_INET, type == lx::kSockDgram ? SOCK_DGRAM : SOCK_STREAM, 0);
  if (h != INVALID_SOCKET) {
    int size = kUnixBuffer;
    setsockopt(h, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&size), sizeof size);
    setsockopt(h, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&size), sizeof size);
  }
  return h;
}

// connect(2) of an AF_UNIX socket: 0, else the Linux errno (kClosedWhileWaiting
// when the socket was closed meanwhile). A stream connection to a listening
// socket of the process is made at once, as Linux makes it (no EINPROGRESS;
// EAGAIN when a non-blocking one finds the backlog full); a datagram socket
// only takes its peer.
constexpr int32_t kClosedWhileWaiting = -1;
int32_t unix_connect(Runtime& rt, const std::shared_ptr<NetSocket>& s, int32_t fd, const std::string& sun_path) {
  int32_t err = 0;
  std::shared_ptr<NetSocket> target = unix_lookup(sun_path, err);
  if (!target) return err;
  if (target->type != s->type) return lx::kEPROTOTYPE;
  bool stream = s->type != lx::kSockDgram;
  if (stream && s->unix_connected) return lx::kEISCONN;
  if (stream && !target->unix_listening) return lx::kECONNREFUSED;
  if (int32_t e = unix_attach(s)) return e;
  Endpoint to = unix_endpoint(*target);
  if (connect(s->handle(), reinterpret_cast<const sockaddr*>(&to.ss), to.len) == SOCKET_ERROR) {
    int wsa = WSAGetLastError();
    if (wsa != WSAEWOULDBLOCK) return linux_errno(wsa);
    switch (wait_socket(rt, *s, fd, POLLWRNORM, s->nonblocking ? 1000000 : 0)) {
      case Wait::kClosed: return kClosedWhileWaiting;
      case Wait::kTimeout: return lx::kEAGAIN;
      case Wait::kReady:
      case Wait::kInterrupted: break;
    }
    if (int failed = pending_error(s->handle())) return linux_errno(failed);
  }
  s->unix_peer = target->unix_name;
  s->unix_connected = true;
  return 0;
}

// The destination of a send with an address on AF_UNIX socket `s`: for a
// datagram socket the named one's loopback endpoint (unix_dgram_sendmsg); a
// stream socket fails, EISCONN when connected, else EOPNOTSUPP
// (unix_stream_sendmsg); SOCK_SEQPACKET ignores the address once connected,
// ENOTCONN before (unix_seqpacket_sendmsg).
Endpoint unix_destination(Runtime& rt, const std::shared_ptr<NetSocket>& s, Handle address, const char* function) {
  if (!is_unix_socket_address(rt, address)) throw_errno(rt, function, lx::kEINVAL);
  std::string sun_path = unix_sun_path(rt, address);
  if (s->type == lx::kSockSeqpacket) {
    if (!s->unix_connected) throw_errno(rt, function, lx::kENOTCONN);
    return Endpoint{};
  }
  if (s->type != lx::kSockDgram) throw_errno(rt, function, s->unix_connected ? lx::kEISCONN : lx::kEOPNOTSUPP);
  int32_t err = 0;
  std::shared_ptr<NetSocket> target = unix_lookup(sun_path, err);
  if (!target) throw_errno(rt, function, err);
  if (target->type != lx::kSockDgram) throw_errno(rt, function, lx::kEPROTOTYPE);
  if (int32_t e = unix_attach(s)) throw_errno(rt, function, e);
  return unix_endpoint(*target);
}

// accept(2) on an AF_UNIX socket: a connection from a port that is none of
// the process's AF_UNIX sockets is closed unseen, as if none had come
// (WSAEWOULDBLOCK). The Winsock socket, else INVALID_SOCKET with the error set.
SOCKET unix_accept(NetSocket& listener, std::shared_ptr<NetSocket>& client) {
  sockaddr_storage ss{};
  int len = sizeof ss;
  SOCKET c = accept(listener.handle(), reinterpret_cast<sockaddr*>(&ss), &len);
  if (c == INVALID_SOCKET) return c;
  client = unix_by_address(listener.type, ss);
  if (!client) {
    closesocket(c);
    WSASetLastError(WSAEWOULDBLOCK);
    return INVALID_SOCKET;
  }
  return c;
}

// The socket of a connection unix_accept took: named as the listener, its
// peer's name the client's.
std::shared_ptr<NetSocket> unix_accepted(SOCKET c, const NetSocket& listener, const NetSocket& client) {
  u_long on = 1;
  ioctlsocket(c, FIONBIO, &on);
  int size = kUnixBuffer;
  setsockopt(c, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&size), sizeof size);
  setsockopt(c, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&size), sizeof size);
  auto sock = std::make_shared<NetSocket>(c, lx::kAfUnix, listener.type, 0, AF_INET);
  sock->unix_name = listener.unix_name;
  sock->unix_peer = client.unix_name;
  sock->unix_connected = true;
  return sock;
}

// One SOCK_SEQPACKET record out, whole: a non-blocking socket without room
// sends nothing (WSAEWOULDBLOCK); a record started is finished, the GIL
// released (`rt` null: waiting in place) while the peer makes room. A record
// longer than the send buffer fails with EMSGSIZE, as on Linux.
int64_t seq_send(Runtime* rt, NetSocket& s, const WSABUF* bufs, DWORD count) {
  size_t total = 0;
  for (DWORD k = 0; k < count; ++k) total += bufs[k].len;
  if (total > size_t(kUnixBuffer) - 32) {
    WSASetLastError(WSAEMSGSIZE);
    return -1;
  }
  std::vector<char> record(kSeqHeader + total);
  uint32_t length = uint32_t(total);
  std::memcpy(record.data(), &length, kSeqHeader);
  size_t at = kSeqHeader;
  for (DWORD k = 0; k < count; ++k) {
    if (bufs[k].len) std::memcpy(record.data() + at, bufs[k].buf, bufs[k].len);
    at += bufs[k].len;
  }
  int sent = send(s.handle(), record.data(), int(record.size()), 0);
  if (sent == SOCKET_ERROR) return -1;
  size_t done = size_t(sent);
  int failure = 0;
  auto finish = [&] {
    while (done < record.size() && !(rt && rt->shutting_down())) {
      WSAPOLLFD p{};
      p.fd = s.handle();
      p.events = POLLWRNORM;
      WSAPoll(&p, 1, 50);
      int more = send(s.handle(), record.data() + done, int(record.size() - done), 0);
      if (more > 0) {
        done += size_t(more);
      } else if (more == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        failure = WSAGetLastError();
        return;
      }
    }
  };
  if (done < record.size()) {
    if (rt) {
      rt->run_blocking(finish);
    } else {
      finish();
    }
  }
  if (done < record.size()) {
    WSASetLastError(failure ? failure : WSAESHUTDOWN);
    return -1;
  }
  return int64_t(total);
}

// One SOCK_SEQPACKET record in, whole, or WSAEWOULDBLOCK while it is not all
// there: the bytes that fit in `bufs`, the rest dropped (`truncated`), as
// Linux does. 0 at the end of the connection.
int64_t seq_recv(NetSocket& s, WSABUF* bufs, DWORD count, bool peek, bool& truncated) {
  u_long avail = 0;
  if (ioctlsocket(s.handle(), FIONREAD, &avail) == SOCKET_ERROR) return -1;
  uint32_t length = 0;
  if (avail < kSeqHeader || recv(s.handle(), reinterpret_cast<char*>(&length), int(kSeqHeader), MSG_PEEK) != int(kSeqHeader) ||
      avail < kSeqHeader + length) {
    if (avail == 0) {
      char c;
      int r = recv(s.handle(), &c, 1, MSG_PEEK);  // 0 at the end of the connection, else an error or nothing yet
      if (r == 0) return 0;
      if (r == SOCKET_ERROR) return -1;
    }
    WSASetLastError(WSAEWOULDBLOCK);
    return -1;
  }
  std::vector<char> record(kSeqHeader + length);
  if (recv(s.handle(), record.data(), int(record.size()), peek ? MSG_PEEK : 0) != int(record.size())) return -1;
  size_t copied = 0;
  for (DWORD k = 0; k < count && copied < length; ++k) {
    size_t n = std::min<size_t>(bufs[k].len, length - copied);
    std::memcpy(bufs[k].buf, record.data() + kSeqHeader + copied, n);
    copied += n;
  }
  truncated = copied < length;
  return int64_t(copied);
}

// One receive into `bufs` with the Winsock flags `flags`, AF_UNIX's semantics
// included: a SOCK_SEQPACKET record whole; a datagram from another program of
// the host dropped. `from` (with its length) gets the sender of an Internet
// socket, `unix_from` the name of an AF_UNIX socket's sender. -1 with the
// Winsock error set on failure; a datagram cut short sets `truncated`.
int64_t recv_into(NetSocket& s, WSABUF* bufs, DWORD count, int flags, sockaddr_storage* from, int* from_len,
                  bool& truncated, std::string* unix_from = nullptr) {
  truncated = false;
  if (s.family == lx::kAfUnix && s.type != lx::kSockDgram) {
    if (unix_from) *unix_from = s.unix_peer;
    if (s.type == lx::kSockSeqpacket) return seq_recv(s, bufs, count, (flags & MSG_PEEK) != 0, truncated);
  }
  bool unix_dgram = s.family == lx::kAfUnix && s.type == lx::kSockDgram;
  sockaddr_storage source{};
  int source_len = sizeof source;
  sockaddr* name = unix_dgram ? reinterpret_cast<sockaddr*>(&source) : reinterpret_cast<sockaddr*>(from);
  int* name_len = unix_dgram ? &source_len : from_len;
  size_t total = 0;
  for (DWORD k = 0; k < count; ++k) total += bufs[k].len;
  DWORD got = 0, f = DWORD(flags);
  int64_t n;
  if (WSARecvFrom(s.handle(), bufs, count, &got, &f, name, name_len, nullptr, nullptr) == SOCKET_ERROR) {
    if (WSAGetLastError() != WSAEMSGSIZE) return -1;
    truncated = true;
    n = int64_t(total);
  } else {
    n = int64_t(got);
  }
  if (unix_dgram) {
    std::shared_ptr<NetSocket> sender = unix_by_address(lx::kSockDgram, source);
    if (!sender) {
      if (flags & MSG_PEEK) {
        char c;
        recv(s.handle(), &c, 1, 0);  // takes the peeked datagram away
      }
      WSASetLastError(WSAEWOULDBLOCK);
      return -1;
    }
    if (unix_from) *unix_from = sender->unix_name;
  }
  return n;
}

// One send of `bufs` (to `to` if not null) with the Winsock flags `flags`; a
// SOCK_SEQPACKET record whole. -1 with the Winsock error set on failure.
int64_t send_from(Runtime* rt, NetSocket& s, const WSABUF* bufs, DWORD count, int flags, const Endpoint* to = nullptr) {
  if (s.family == lx::kAfUnix && s.type == lx::kSockSeqpacket) return seq_send(rt, s, bufs, count);
  DWORD sent = 0;
  int rc = WSASendTo(s.handle(), const_cast<WSABUF*>(bufs), count, &sent, DWORD(flags),
                     to ? reinterpret_cast<const sockaddr*>(&to->ss) : nullptr, to ? to->len : 0, nullptr, nullptr);
  return rc == SOCKET_ERROR ? -1 : int64_t(sent);
}

WSABUF wsabuf(std::span<const uint8_t> data) {
  WSABUF w;
  w.len = ULONG(std::min<size_t>(data.size(), INT32_MAX));
  w.buf = const_cast<char*>(reinterpret_cast<const char*>(data.data()));
  return w;
}

size_t NetSocket::read(uint64_t, std::span<uint8_t> out) {
  WSABUF w = wsabuf(out);
  bool truncated = false;
  int64_t n = recv_into(*this, &w, 1, 0, nullptr, nullptr, truncated);
  if (n < 0) throw vfs::VfsError{linux_errno(WSAGetLastError()), "read"};
  return size_t(n);
}

size_t NetSocket::write(uint64_t, std::span<const uint8_t> data) {
  WSABUF w = wsabuf(data);
  int64_t n = send_from(nullptr, *this, &w, 1, 0);
  if (n < 0) throw vfs::VfsError{linux_errno(WSAGetLastError(), true), "write"};
  return size_t(n);
}

// recvmsg(2) into a StructMsghdr: msg_name filled with the sender,
// msg_flags with MSG_TRUNC for a datagram cut short; no ancillary data.
// `throwing`: recvmsg (ErrnoException, SocketException when a close is
// signalled); otherwise recvmsgNoThrow (-errno).
int32_t recv_message(Runtime& rt, Handle java_fd, Handle msg, int32_t flags, bool throwing) {
  auto bufs = message_buffers(rt, msg, true);
  Handle name = field_of(rt, msg, kStructMsghdr, "msg_name", "Ljava/net/SocketAddress;");
  int32_t fd = fd_of(rt, java_fd);
  auto s = socket_of(fd);
  if (!s || s->pipe != NetSocket::Pipe::kNone) {
    int32_t err = fd < 0 || (fd > 2 && !libcore_vfs().is_open(fd)) ? lx::kEBADF : lx::kENOTSOCK;
    if (throwing) throw_errno(rt, "recvmsg", err);
    return -err;
  }
  size_t total = 0;
  for (const WSABUF& w : bufs) total += w.len;
  sockaddr_storage ss{};
  int len = sizeof ss;
  bool truncated = false;
  std::string unix_from;
  auto call = [&]() -> int64_t {
    len = sizeof ss;
    return recv_into(*s, bufs.data(), DWORD(bufs.size()), win_msg_flags(flags), name != kNull ? &ss : nullptr,
                     name != kNull ? &len : nullptr, truncated, &unix_from);
  };
  bool dontwait = (flags & lx::kMsgDontwait) != 0;
  int64_t n;
  if (throwing) {
    n = retry(rt, *s, fd, "recvmsg", POLLRDNORM, s->rcvtimeo_us, dontwait, false, false, call);
  } else {
    int32_t err = 0;
    n = retry_nothrow(rt, *s, fd, POLLRDNORM, s->rcvtimeo_us, dontwait, false, err, call);
    if (n < 0) return -err;
  }
  if (name != kNull && s->family == lx::kAfUnix) {
    if (!is_unix_socket_address(rt, name)) {
      if (throwing) rt.throw_new("Ljava/lang/UnsupportedOperationException;", "unsupported SocketAddress subclass");
    } else {
      fill_unix_address(rt, name, unix_from);
    }
  } else if (name != kNull) {
    bool inet = ss.ss_family == AF_INET || ss.ss_family == AF_INET6;
    if (throwing && !inet) throw_net(rt, "Ljava/net/SocketException;", "unknown socket address type");
    if (inet) {
      if (!is_inet_socket_address(rt, name)) {
        rt.throw_new("Ljava/lang/UnsupportedOperationException;", "unsupported SocketAddress subclass");
      }
      fill_inet_socket_address(rt, name, reinterpret_cast<const sockaddr*>(&ss));
    }
  }
  constexpr int32_t kMsgTrunc = 0x20;
  field_of(rt, msg, kStructMsghdr, "msg_flags", "I") = uint32_t(truncated ? kMsgTrunc : 0);
  return int32_t(n);
}

}  // namespace

std::optional<size_t> socket_read(Runtime& rt, int32_t fd, std::span<uint8_t> out) {
  auto s = socket_of(fd);
  if (!s) return std::nullopt;
  if (wrong_pipe_end(*s, false)) throw_errno(rt, "read", lx::kEBADF);
  WSABUF w = wsabuf(out);
  bool truncated = false;
  return size_t(retry(rt, *s, fd, "read", POLLRDNORM, s->rcvtimeo_us, false, true, false, [&]() -> int64_t {
    return recv_into(*s, &w, 1, 0, nullptr, nullptr, truncated);
  }));
}

std::optional<size_t> socket_write(Runtime& rt, int32_t fd, std::span<const uint8_t> data) {
  auto s = socket_of(fd);
  if (!s) return std::nullopt;
  if (wrong_pipe_end(*s, true)) throw_errno(rt, "write", lx::kEBADF);
  WSABUF w = wsabuf(data);
  return size_t(retry(rt, *s, fd, "write", POLLWRNORM, s->sndtimeo_us, false, true, true, [&]() -> int64_t {
    return send_from(&rt, *s, &w, 1, 0);
  }));
}

std::optional<int64_t> socket_read_nothrow(Runtime& rt, int32_t fd, std::span<uint8_t> out, int32_t& err) {
  auto s = socket_of(fd);
  if (!s) return std::nullopt;
  if (wrong_pipe_end(*s, false)) {
    err = lx::kEBADF;
    return -1;
  }
  WSABUF w = wsabuf(out);
  bool truncated = false;
  return retry_nothrow(rt, *s, fd, POLLRDNORM, s->rcvtimeo_us, false, false, err, [&]() -> int64_t {
    return recv_into(*s, &w, 1, 0, nullptr, nullptr, truncated);
  });
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
    if (std::find(w->fds.begin(), w->fds.end(), fd) != w->fds.end()) w->signal = Wait::kClosed;
  }
}

std::vector<std::span<uint8_t>> native_iovecs(Runtime& rt, int64_t address, int32_t count, bool write) {
  std::vector<std::span<uint8_t>> out;
  if (count <= 0) return out;
  // struct iovec of x86_64: base (8 bytes), length (8 bytes).
  std::vector<uint8_t> iov(size_t(count) * 16);
  auto raw = native_span(rt, address, iov.size(), false);
  std::memcpy(iov.data(), raw.data(), iov.size());
  for (int32_t i = 0; i < count; ++i) {
    uint64_t base = 0, length = 0;
    std::memcpy(&base, iov.data() + size_t(i) * 16, 8);
    std::memcpy(&length, iov.data() + size_t(i) * 16 + 8, 8);
    if (length == 0) continue;
    if (length > INT32_MAX) rt.throw_new("Ljava/lang/IndexOutOfBoundsException;", "iovec");
    out.push_back(native_span(rt, int64_t(base), length, write));
  }
  return out;
}

namespace {

// --- NIO (sun.nio.ch of ojluni: Net.c, IOUtil.c, ServerSocketChannelImpl.c,
// SocketChannelImpl.c, PollArrayWrapper.c, DatagramChannelImpl.c, ...) -------------------

// sun.nio.ch.IOStatus.
constexpr int32_t kIosEof = -1, kIosUnavailable = -2, kIosInterrupted = -3, kIosThrown = -5;
constexpr int32_t kEproto = 71;
constexpr int32_t kMaxPacketLen = 65536;  // DatagramChannelImpl.c MAX_PACKET_LEN

// Net.c handleSocketErrorWithDefault: the exception of a socket call's
// errno, carrying strerror's text (JNU_ThrowByNameWithLastError).
[[noreturn]] void throw_socket_error(Runtime& rt, int32_t err, const char* fallback = "Ljava/net/SocketException;") {
  const char* cls = fallback;
  switch (err) {
    case kEproto: cls = "Ljava/net/ProtocolException;"; break;
    case lx::kECONNREFUSED:
    case lx::kETIMEDOUT: cls = "Ljava/net/ConnectException;"; break;
    case lx::kEHOSTUNREACH: cls = "Ljava/net/NoRouteToHostException;"; break;
    case lx::kEADDRINUSE:
    case lx::kEADDRNOTAVAIL: cls = "Ljava/net/BindException;"; break;
    default: break;
  }
  throw_net(rt, cls, strerror_text(err));
}

// JNU_ThrowIOExceptionWithLastError: IOException carrying strerror's text.
[[noreturn]] void throw_io_errno(Runtime& rt, int32_t err) { throw_net(rt, "Ljava/io/IOException;", strerror_text(err)); }

[[noreturn]] void throw_port_unreachable(Runtime& rt) {
  throw rt::JavaThrow{construct(rt, "Ljava/net/PortUnreachableException;", "()V", {})};
}

// The socket of a FileDescriptor for NIO's natives (fdval): EBADF or ENOTSOCK
// as handleSocketError reports them.
std::shared_ptr<NetSocket> nio_socket(Runtime& rt, Handle java_fd, int32_t& fd) {
  fd = fd_of(rt, java_fd);
  auto s = socket_of(fd);
  if (!s || s->pipe != NetSocket::Pipe::kNone) {
    throw_socket_error(rt, fd > 2 && libcore_vfs().is_open(fd) ? lx::kENOTSOCK : lx::kEBADF);
  }
  return s;
}

// NET_InetAddressToSockaddr: an IPv4 address mapped for an IPv6 socket
// (`prefer6`), plain otherwise; an IPv6 address needs IPv6.
Endpoint nio_endpoint(Runtime& rt, NetSocket& s, Handle inet, int32_t port, bool prefer6) {
  Endpoint e = to_sockaddr(rt, inet, port, prefer6 && ipv6_available());
  if (e.ss.ss_family == AF_INET6 && !ipv6_available()) {
    throw_net(rt, "Ljava/net/SocketException;", "Protocol family unavailable");
  }
  Endpoint v4;
  if (e.ss.ss_family == AF_INET6 && s.win_af == AF_INET && to_ipv4(e, v4)) return v4;
  return e;
}

// A socket call of NIO, as its natives see read(2), accept(2) and the like
// on Linux: its result; IOS_UNAVAILABLE when a non-blocking socket would
// block (EAGAIN, also when SO_RCVTIMEO or SO_SNDTIMEO expire);
// IOS_INTERRUPTED when NativeThread.signal interrupts the wait of a
// blocking one (EINTR); else IOS_THROWN, the Linux errno in `err`. The call
// returns -1 with the Winsock error set on failure.
template <typename Call>
int64_t nio_call(Runtime& rt, NetSocket& s, int32_t fd, short events, bool sending, int32_t& err, Call call) {
  for (;;) {
    int64_t r = call();
    if (r >= 0) return r;
    int wsa = WSAGetLastError();
    if (wsa == WSAEINTR) return kIosInterrupted;
    if (wsa != WSAEWOULDBLOCK) {
      err = io_errno(s, wsa, sending);
      return kIosThrown;
    }
    if (s.nonblocking) return kIosUnavailable;
    switch (wait_socket(rt, s, fd, events, sending ? s.sndtimeo_us : s.rcvtimeo_us, true)) {
      case Wait::kReady: break;
      case Wait::kTimeout: return kIosUnavailable;
      case Wait::kClosed:
      case Wait::kInterrupted: return kIosInterrupted;
    }
  }
}

// read(2)/readv(2) or write(2)/writev(2) of NIO on a socket or pipe, the
// result as IOUtil.c's convertReturnVal makes it (0 bytes read: IOS_EOF).
// A datagram longer than the buffers is cut, as Linux does; ECONNREFUSED
// (an earlier datagram's ICMP error) is PortUnreachableException.
int64_t nio_io(Runtime& rt, NetSocket& s, int32_t fd, std::span<const std::span<uint8_t>> buffers, bool reading) {
  if (wrong_pipe_end(s, !reading)) throw_io_errno(rt, lx::kEBADF);
  std::vector<WSABUF> bufs;
  size_t total = 0;
  for (std::span<uint8_t> b : buffers) {
    if (b.empty()) continue;
    WSABUF w;
    w.len = ULONG(std::min<size_t>(b.size(), INT32_MAX - total));
    w.buf = reinterpret_cast<char*>(b.data());
    bufs.push_back(w);
    total += w.len;
    if (total >= INT32_MAX) break;
  }
  int32_t err = 0;
  int64_t n = 0;
  if (!bufs.empty()) {
    n = nio_call(rt, s, fd, reading ? POLLRDNORM : POLLWRNORM, !reading, err, [&]() -> int64_t {
      bool truncated = false;
      return reading ? recv_into(s, bufs.data(), DWORD(bufs.size()), 0, nullptr, nullptr, truncated)
                     : send_from(&rt, s, bufs.data(), DWORD(bufs.size()), 0);
    });
  }
  if (n == kIosThrown) {
    if (err == lx::kECONNREFUSED && s.type == lx::kSockDgram) throw_port_unreachable(rt);
    throw_io_errno(rt, err);
  }
  if (n == 0 && reading) return kIosEof;
  return n;
}

// poll(2) for NIO, the GIL released while it waits (timeout_ms < 0: for
// ever); the number of ready entries. An `interruptible` poll (Net.poll,
// checkConnect) stops on NativeThread.signal and returns IOS_INTERRUPTED
// (EINTR); the selector's (PollArrayWrapper.poll0) restarts, as RESTARTABLE
// and ipoll do.
int poll_wait(Runtime& rt, std::vector<PollEntry>& entries, int64_t timeout_ms, bool interruptible) {
  resolve_poll(entries);
  int ready = poll_once(entries, 0);
  if (ready > 0 || timeout_ms == 0) return ready;
  std::vector<int32_t> fds;
  for (const PollEntry& e : entries) fds.push_back(e.fd);
  bool any_socket = std::any_of(entries.begin(), entries.end(), [](const PollEntry& e) { return e.sock != nullptr; });
  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + std::chrono::milliseconds(std::max<int64_t>(timeout_ms, 0));
  bool interrupted = false;
  WaitRegistration registration(std::move(fds), interruptible ? current_thread_id(rt) : 0);
  rt.run_blocking([&] {
    for (;;) {
      if (registration.signal() == Wait::kInterrupted) {
        interrupted = true;
        return;
      }
      if (rt.shutting_down()) return;
      int slice = 50;
      if (timeout_ms > 0) {  // never before the timeout: the last slice rounded up
        auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now()).count();
        if (left <= 0) return;
        slice = int(std::min<int64_t>(slice, (left + 999) / 1000));
      }
      if (!any_socket) {
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        continue;
      }
      ready = poll_once(entries, slice);
      if (ready > 0) return;
    }
  });
  return interrupted ? kIosInterrupted : ready;
}

// pipe(2): the descriptors of its read and write ends, else the Linux errno.
int32_t make_pipe(bool nonblocking, int32_t fds[2]) {
  ensure_winsock();
  auto [one, two] = loopback_pair(SOCK_STREAM);
  if (one == INVALID_SOCKET) return linux_errno(WSAGetLastError());
  auto read_end = std::make_shared<NetSocket>(one, lx::kAfUnix, lx::kSockStream, 0, AF_INET, NetSocket::Pipe::kRead);
  auto write_end = std::make_shared<NetSocket>(two, lx::kAfUnix, lx::kSockStream, 0, AF_INET, NetSocket::Pipe::kWrite);
  read_end->nonblocking = nonblocking;
  write_end->nonblocking = nonblocking;
  if (int32_t err = install_fd(std::move(read_end), fds[0])) return err;
  if (int32_t err = install_fd(std::move(write_end), fds[1])) {
    libcore_vfs().close(fds[0]);
    return err;
  }
  return 0;
}

// One read(2) or write(2) that never waits (IOUtil.drain, the selector's
// wakeup byte): bytes, or -1 and the Linux errno.
int64_t once(int32_t fd, std::span<uint8_t> data, bool sending, int32_t& err) {
  if (auto s = socket_of(fd)) {
    if (wrong_pipe_end(*s, sending)) {
      err = lx::kEBADF;
      return -1;
    }
    WSABUF w = wsabuf(data);
    bool truncated = false;
    int64_t n = sending ? send_from(nullptr, *s, &w, 1, 0) : recv_into(*s, &w, 1, 0, nullptr, nullptr, truncated);
    if (n < 0) {
      err = io_errno(*s, WSAGetLastError(), sending);
      return -1;
    }
    return n;
  }
  try {
    return int64_t(sending ? write_fd(fd, data) : read_fd(fd, data));
  } catch (const vfs::VfsError& e) {
    err = e.err;
    return -1;
  }
}

void set_descriptor(Runtime& rt, Handle java_fd, int32_t fd) {
  if (java_fd == kNull) rt.throw_npe({});
  field_of(rt, java_fd, "Ljava/io/FileDescriptor;", "descriptor", "I") = uint32_t(fd);
}

// NET_SockaddrEqualsInetAddress: the same address (an IPv4-mapped one equals its IPv4 address).
bool same_address(Runtime& rt, const sockaddr_storage& ss, Handle inet) {
  Endpoint e = to_sockaddr(rt, inet, 0, false);
  auto bytes = [](const sockaddr_storage& a, std::vector<uint8_t>& out, uint32_t& scope) {
    if (a.ss_family == AF_INET) {
      const auto* p = reinterpret_cast<const uint8_t*>(&reinterpret_cast<const sockaddr_in&>(a).sin_addr);
      out.assign(p, p + 4);
    } else if (a.ss_family == AF_INET6) {
      const auto& sin6 = reinterpret_cast<const sockaddr_in6&>(a);
      if (IN6_IS_ADDR_V4MAPPED(&sin6.sin6_addr)) {
        out.assign(&sin6.sin6_addr.s6_addr[12], &sin6.sin6_addr.s6_addr[16]);
      } else {
        out.assign(sin6.sin6_addr.s6_addr, sin6.sin6_addr.s6_addr + 16);
        scope = sin6.sin6_scope_id;
      }
    }
  };
  std::vector<uint8_t> a, b;
  uint32_t scope_a = 0, scope_b = 0;
  bytes(ss, a, scope_a);
  bytes(e.ss, b, scope_b);
  return !a.empty() && a == b && scope_a == scope_b;
}

int32_t port_of(const sockaddr_storage& ss) {
  if (ss.ss_family == AF_INET) return ntohs(reinterpret_cast<const sockaddr_in&>(ss).sin_port);
  if (ss.ss_family == AF_INET6) return ntohs(reinterpret_cast<const sockaddr_in6&>(ss).sin6_port);
  return 0;
}

// getsockname or getpeername of a NIO socket (an unbound one: the wildcard
// address, port 0, as Linux names it).
sockaddr_storage nio_name(Runtime& rt, Handle java_fd, bool peer) {
  int32_t fd = -1;
  auto s = nio_socket(rt, java_fd, fd);
  sockaddr_storage ss{};
  int len = sizeof ss;
  int rc = peer ? getpeername(s->handle(), reinterpret_cast<sockaddr*>(&ss), &len)
                : getsockname(s->handle(), reinterpret_cast<sockaddr*>(&ss), &len);
  if (rc == SOCKET_ERROR) {
    int err = WSAGetLastError();
    if (peer || err != WSAEINVAL) throw_socket_error(rt, linux_errno(err));
    std::memset(&ss, 0, sizeof ss);
    ss.ss_family = ADDRESS_FAMILY(s->win_af);
  }
  return ss;
}

// A setsockopt of Net.c's multicast natives: IOS_UNAVAILABLE when a join or
// block is not supported (ENOPROTOOPT, EOPNOTSUPP), else 0.
int32_t multicast_option(Runtime& rt, NetSocket& s, int level, int option, const void* value, int len, bool adding) {
  if (setsockopt(s.handle(), level, option, static_cast<const char*>(value), len) == SOCKET_ERROR) {
    int32_t err = linux_errno(WSAGetLastError());
    if (adding && (err == lx::kENOPROTOOPT || err == lx::kEOPNOTSUPP)) return kIosUnavailable;
    throw_socket_error(rt, err);
  }
  return 0;
}

std::array<uint8_t, 16> inet6_bytes(Runtime& rt, Handle bytes) {
  std::array<uint8_t, 16> out{};
  rt::ArrayObject& b = rt.array(bytes, "byte[]");
  if (b.length < 16) rt.throw_new("Ljava/lang/IllegalArgumentException;", "adresse IPv6 tronquée");
  std::memcpy(out.data(), b.data.data(), 16);
  return out;
}

void register_nio_net(rt::NativeRegistry& r) {
  const char* net = "Lsun/nio/ch/Net;";  // static natives
  r.bind(net, "isIPv6Available0", "()Z", [](Runtime&, NativeArgs&) -> uint64_t { return ipv6_available() ? 1 : 0; });
  r.bind(net, "isExclusiveBindAvailable", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_int(-1); });
  r.bind(net, "canIPv6SocketJoinIPv4Group0", "()Z", [](Runtime&, NativeArgs&) -> uint64_t { return 1; });
  r.bind(net, "canJoin6WithIPv4Group0", "()Z", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  for (auto [name, value] : {std::pair{"pollinValue", lx::kPollIn}, std::pair{"polloutValue", lx::kPollOut},
                             std::pair{"pollerrValue", lx::kPollErr}, std::pair{"pollhupValue", lx::kPollHup},
                             std::pair{"pollnvalValue", lx::kPollNval}, std::pair{"pollconnValue", lx::kPollOut}}) {
    int16_t v = value;
    r.bind(net, name, "()S", [v](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_int(v); });
  }
  r.bind(net, "socket0", "(ZZZZ)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    bool prefer6 = a.i32(0) != 0, stream = a.i32(1) != 0, reuse = a.i32(2) != 0;
    if (!libcore_config().internet) throw_socket_error(rt, lx::kEACCES);
    int af = prefer6 && ipv6_available() ? AF_INET6 : AF_INET;
    SOCKET h = new_win_socket(af, stream ? SOCK_STREAM : SOCK_DGRAM, 0);
    int32_t fd = -1, err = h == INVALID_SOCKET ? linux_errno(WSAGetLastError()) : 0;
    if (!err) {
      auto sock = std::make_shared<NetSocket>(h, linux_family(af), stream ? lx::kSockStream : lx::kSockDgram, 0, af);
      if (reuse) set_option(*sock, lx::kSolSocket, lx::kSoReuseaddr, 1);
      if (!stream) sock->multicast_all = 0;  // IP_MULTICAST_ALL off
      if (!stream && af == AF_INET6) set_option(*sock, lx::kIpprotoIpv6, lx::kIpv6MulticastHops, 1);  // not the route's
      err = install_fd(std::move(sock), fd);
    }
    // tagSocket (socket_tagger_util.cpp): SocketTagger.get().tag(fd), the
    // descriptor -1 when socket(2) failed.
    rt::GlobalRef java_fd(rt, new_file_descriptor(rt, err ? -1 : fd));
    Handle tagger = Handle(rt.call_static("Ldalvik/system/SocketTagger;", "get", "()Ldalvik/system/SocketTagger;", {}));
    if (tagger != kNull) rt.call_virtual(tagger, "tag", "(Ljava/io/FileDescriptor;)V", {java_fd.get()});
    if (err) throw_socket_error(rt, err);
    return rt::ret_int(fd);
  }, S::kPartial, "AF_INET et AF_INET6 (TCP, UDP) sur Winsock");
  r.bind(net, "bind0", "(Ljava/io/FileDescriptor;ZZLjava/net/InetAddress;I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = -1;
    auto s = nio_socket(rt, a.h(0), fd);
    Endpoint e = nio_endpoint(rt, *s, a.h(3), a.i32(4), a.i32(1) != 0);
    if (bind(s->handle(), reinterpret_cast<const sockaddr*>(&e.ss), e.len) == SOCKET_ERROR) {
      throw_socket_error(rt, linux_errno(WSAGetLastError()));
    }
    return 0;
  });
  r.bind(net, "listen", "(Ljava/io/FileDescriptor;I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = -1;
    auto s = nio_socket(rt, a.h(0), fd);
    int backlog = a.i32(1) < 0 ? SOMAXCONN : a.i32(1);  // Linux takes a negative backlog as unsigned, then the maximum
    if (listen(s->handle(), backlog) == SOCKET_ERROR) throw_socket_error(rt, linux_errno(WSAGetLastError()));
    return 0;
  });
  // connect0: 1 once connected; IOS_UNAVAILABLE for a non-blocking socket
  // (EINPROGRESS); a blocking one waits, IOS_INTERRUPTED if signalled.
  r.bind(net, "connect0", "(ZLjava/io/FileDescriptor;Ljava/net/InetAddress;I)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = -1;
    auto s = nio_socket(rt, a.h(1), fd);
    Endpoint e = nio_endpoint(rt, *s, a.h(2), a.i32(3), a.i32(0) != 0);
    if (connect(s->handle(), reinterpret_cast<const sockaddr*>(&e.ss), e.len) == 0) return rt::ret_int(1);
    int err = WSAGetLastError();
    if (err == WSAEINVAL && s->connecting) err = WSAEALREADY;
    constexpr const char* kConnectException = "Ljava/net/ConnectException;";
    if (err == WSAEINTR) return rt::ret_int(kIosInterrupted);
    if (err != WSAEWOULDBLOCK) throw_socket_error(rt, linux_errno(err), kConnectException);
    s->connecting = true;
    if (s->nonblocking) return rt::ret_int(kIosUnavailable);
    if (wait_socket(rt, *s, fd, POLLWRNORM, 0, true) != Wait::kReady) return rt::ret_int(kIosInterrupted);
    s->connecting = false;
    if (int failed = pending_error(s->handle())) throw_socket_error(rt, linux_errno(failed), kConnectException);
    return rt::ret_int(1);
  });
  r.bind(net, "localInetAddress", "(Ljava/io/FileDescriptor;)Ljava/net/InetAddress;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    sockaddr_storage ss = nio_name(rt, a.h(0), false);
    return to_inet_address(rt, reinterpret_cast<const sockaddr*>(&ss), nullptr);
  });
  r.bind(net, "localPort", "(Ljava/io/FileDescriptor;)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(port_of(nio_name(rt, a.h(0), false)));
  });
  r.bind(net, "remoteInetAddress", "(Ljava/io/FileDescriptor;)Ljava/net/InetAddress;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    sockaddr_storage ss = nio_name(rt, a.h(0), true);
    return to_inet_address(rt, reinterpret_cast<const sockaddr*>(&ss), nullptr);
  });
  r.bind(net, "remotePort", "(Ljava/io/FileDescriptor;)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(port_of(nio_name(rt, a.h(0), true)));
  });
  // Options: SO_LINGER as its time or -1, IP_MULTICAST_TTL and
  // IP_MULTICAST_LOOP as a byte; SO_SNDBUF and SO_RCVBUF as set (Linux
  // doubles them and NET_GetSockOpt halves them back).
  r.bind(net, "getIntOption0", "(Ljava/io/FileDescriptor;ZII)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = -1;
    auto s = nio_socket(rt, a.h(0), fd);
    int32_t level = a.i32(2), option = a.i32(3);
    if (level == lx::kSolSocket && option == lx::kSoLinger) {
      LINGER l{};
      int len = sizeof l;
      if (getsockopt(s->handle(), SOL_SOCKET, SO_LINGER, reinterpret_cast<char*>(&l), &len) == SOCKET_ERROR) {
        throw_net(rt, "Ljava/net/SocketException;", strerror_text(linux_errno(WSAGetLastError())));
      }
      return rt::ret_int(l.l_onoff ? int32_t(l.l_linger) : -1);
    }
    int32_t value = 0;
    if (int32_t err = int_option(*s, level, option, value)) throw_net(rt, "Ljava/net/SocketException;", strerror_text(err));
    if (level == lx::kIpprotoIp && (option == lx::kIpMulticastTtl || option == lx::kIpMulticastLoop)) value &= 0xff;
    return rt::ret_int(value);
  });
  r.bind(net, "setIntOption0", "(Ljava/io/FileDescriptor;ZIIIZ)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = -1;
    auto s = nio_socket(rt, a.h(0), fd);
    int32_t level = a.i32(2), option = a.i32(3), value = a.i32(4);
    bool convert = a.i32(1) != 0, is_ipv6 = a.i32(5) != 0;
    if (level == lx::kSolSocket && option == lx::kSoLinger) {
      LINGER l{};
      l.l_onoff = u_short(value >= 0);
      l.l_linger = u_short(value >= 0 ? std::min(value, 0xffff) : 0);
      if (setsockopt(s->handle(), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof l) == SOCKET_ERROR) {
        throw_net(rt, "Ljava/net/SocketException;", strerror_text(linux_errno(WSAGetLastError())));
      }
      return 0;
    }
    if (level == lx::kIpprotoIp && (option == lx::kIpMulticastTtl || option == lx::kIpMulticastLoop)) value &= 0xff;
    if (convert) {  // NET_SetSockOpt of Linux
      if (level == lx::kSolSocket && option == lx::kSoRcvbuf && value < 1024) value = 1024;
      if (level == lx::kIpprotoIp && option == lx::kIpTos && s->win_af == AF_INET6) {
        set_option(*s, lx::kIpprotoIpv6, lx::kIpv6Tclass, value);  // IPV6_TCLASS too (IPV6_FLOWINFO_SEND: none here)
      }
    }
    if (int32_t err = set_option(*s, level, option, value)) throw_net(rt, "Ljava/net/SocketException;", strerror_text(err));
    if (level == lx::kIpprotoIpv6 && option == lx::kIpv6Tclass && is_ipv6) {
      set_option(*s, lx::kIpprotoIp, lx::kIpTos, value);  // the IPv4 option too, its failure ignored
    }
    return 0;
  });
  r.bind(net, "shutdown", "(Ljava/io/FileDescriptor;I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = -1;
    auto s = nio_socket(rt, a.h(0), fd);
    // Net.SHUT_RD, SHUT_WR, SHUT_RDWR are SD_RECEIVE, SD_SEND, SD_BOTH.
    if (shutdown(s->handle(), a.i32(1)) == SOCKET_ERROR) {
      int32_t err = linux_errno(WSAGetLastError());
      if (err != lx::kENOTCONN) throw_socket_error(rt, err);
    }
    return 0;
  });
  // poll(2) of one descriptor: its revents, IOS_INTERRUPTED if signalled.
  r.bind(net, "poll", "(Ljava/io/FileDescriptor;IJ)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    std::vector<PollEntry> entries(1);
    entries[0].fd = fd_of(rt, a.h(0));
    entries[0].events = int16_t(a.i32(1));
    int rv = poll_wait(rt, entries, int32_t(a.i64(2)), true);  // poll(2) takes an int
    if (rv == kIosInterrupted) return rt::ret_int(kIosInterrupted);
    return rt::ret_int(entries[0].revents);
  });

  // Multicast (Net.c): IPv4 groups by ip_mreq or ip_mreq_source; IPv6 groups
  // by ipv6_mreq, source filtering unavailable as on Linux.
  r.bind(net, "joinOrDrop4", "(ZLjava/io/FileDescriptor;III)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = -1;
    auto s = nio_socket(rt, a.h(1), fd);
    bool join = a.i32(0) != 0;
    uint32_t group = uint32_t(a.i32(2)), interf = uint32_t(a.i32(3)), source = uint32_t(a.i32(4));
    if (source == 0) {
      ip_mreq m{};
      m.imr_multiaddr.s_addr = htonl(group);
      m.imr_interface.s_addr = htonl(interf);
      return rt::ret_int(multicast_option(rt, *s, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, &m, sizeof m, join));
    }
    ip_mreq_source m{};
    m.imr_multiaddr.s_addr = htonl(group);
    m.imr_sourceaddr.s_addr = htonl(source);
    m.imr_interface.s_addr = htonl(interf);
    return rt::ret_int(multicast_option(rt, *s, IPPROTO_IP, join ? IP_ADD_SOURCE_MEMBERSHIP : IP_DROP_SOURCE_MEMBERSHIP, &m,
                                        sizeof m, join));
  });
  r.bind(net, "blockOrUnblock4", "(ZLjava/io/FileDescriptor;III)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = -1;
    auto s = nio_socket(rt, a.h(1), fd);
    bool block = a.i32(0) != 0;
    ip_mreq_source m{};
    m.imr_multiaddr.s_addr = htonl(uint32_t(a.i32(2)));
    m.imr_interface.s_addr = htonl(uint32_t(a.i32(3)));
    m.imr_sourceaddr.s_addr = htonl(uint32_t(a.i32(4)));
    return rt::ret_int(multicast_option(rt, *s, IPPROTO_IP, block ? IP_BLOCK_SOURCE : IP_UNBLOCK_SOURCE, &m, sizeof m, block));
  });
  r.bind(net, "joinOrDrop6", "(ZLjava/io/FileDescriptor;[BI[B)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = -1;
    auto s = nio_socket(rt, a.h(1), fd);
    if (a.h(4) != kNull) return rt::ret_int(kIosUnavailable);  // include-mode filtering, as on Linux
    bool join = a.i32(0) != 0;
    ipv6_mreq m{};
    auto group = inet6_bytes(rt, a.h(2));
    std::memcpy(&m.ipv6mr_multiaddr, group.data(), 16);
    m.ipv6mr_interface = ULONG(a.i32(3));
    return rt::ret_int(multicast_option(rt, *s, IPPROTO_IPV6, join ? IPV6_ADD_MEMBERSHIP : IPV6_DROP_MEMBERSHIP, &m, sizeof m,
                                        join));
  });
  r.bind(net, "blockOrUnblock6", "(ZLjava/io/FileDescriptor;[BI[B)I",
         [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_int(kIosUnavailable); });  // as on Linux
  r.bind(net, "setInterface4", "(Ljava/io/FileDescriptor;I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = -1;
    auto s = nio_socket(rt, a.h(0), fd);
    in_addr in{};
    in.s_addr = htonl(uint32_t(a.i32(1)));
    if (setsockopt(s->handle(), IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&in), sizeof in) == SOCKET_ERROR) {
      throw_socket_error(rt, linux_errno(WSAGetLastError()));
    }
    return 0;
  });
  r.bind(net, "getInterface4", "(Ljava/io/FileDescriptor;)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = -1;
    auto s = nio_socket(rt, a.h(0), fd);
    in_addr in{};
    int len = sizeof in;
    if (getsockopt(s->handle(), IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<char*>(&in), &len) == SOCKET_ERROR) {
      throw_socket_error(rt, linux_errno(WSAGetLastError()));
    }
    return rt::ret_int(int32_t(ntohl(in.s_addr)));
  });
  r.bind(net, "setInterface6", "(Ljava/io/FileDescriptor;I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = -1;
    auto s = nio_socket(rt, a.h(0), fd);
    DWORD index = DWORD(a.i32(1));
    if (setsockopt(s->handle(), IPPROTO_IPV6, IPV6_MULTICAST_IF, reinterpret_cast<const char*>(&index), sizeof index) ==
        SOCKET_ERROR) {
      throw_socket_error(rt, linux_errno(WSAGetLastError()));
    }
    return 0;
  });
  r.bind(net, "getInterface6", "(Ljava/io/FileDescriptor;)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = -1;
    auto s = nio_socket(rt, a.h(0), fd);
    DWORD index = 0;
    int len = sizeof index;
    if (getsockopt(s->handle(), IPPROTO_IPV6, IPV6_MULTICAST_IF, reinterpret_cast<char*>(&index), &len) == SOCKET_ERROR) {
      throw_socket_error(rt, linux_errno(WSAGetLastError()));
    }
    return rt::ret_int(int32_t(index));
  });

  // Channels.
  auto accept0 = [](Runtime& rt, NativeArgs& a) -> uint64_t {  // instance: ssfd, newfd, isaa
    int32_t fd = -1;
    auto s = nio_socket(rt, a.h(1), fd);
    sockaddr_storage ss{};
    int len = sizeof ss;
    SOCKET client = INVALID_SOCKET;
    int32_t err = 0;
    int64_t n = nio_call(rt, *s, fd, POLLRDNORM, false, err, [&]() -> int64_t {
      for (;;) {  // a connection reset before it was accepted (ECONNABORTED): the next one
        len = sizeof ss;
        client = accept(s->handle(), reinterpret_cast<sockaddr*>(&ss), &len);
        if (client != INVALID_SOCKET) return 0;
        if (WSAGetLastError() != WSAECONNABORTED) return -1;
      }
    });
    if (n == kIosThrown) throw_io_errno(rt, err);
    if (n < 0) return rt::ret_int(int32_t(n));
    u_long on = 1;
    ioctlsocket(client, FIONBIO, &on);
    // Blocking, as accept(2) makes it.
    auto sock = std::make_shared<NetSocket>(client, s->family, s->type, s->protocol, s->win_af);
    int32_t newfd = -1;
    if (int32_t e = install_fd(std::move(sock), newfd)) throw_io_errno(rt, e);
    set_descriptor(rt, a.h(2), newfd);
    Handle isa = make_socket_address(rt, reinterpret_cast<const sockaddr*>(&ss));
    Handle isaa = a.h(3);
    if (isaa == kNull) rt.throw_npe({});
    std::memcpy(rt.array(isaa, "InetSocketAddress[]").data.data(), &isa, 4);
    return rt::ret_int(1);
  };
  for (const char* cls : {"Lsun/nio/ch/ServerSocketChannelImpl;", "Lsun/nio/ch/UnixAsynchronousServerSocketChannelImpl;"}) {
    r.bind(cls, "initIDs", "()V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
    r.bind(cls, "accept0", "(Ljava/io/FileDescriptor;Ljava/io/FileDescriptor;[Ljava/net/InetSocketAddress;)I", accept0);
  }

  // UnixDomainSockets (UnixDomainSockets.c): the AF_UNIX stream sockets of
  // SocketChannel and ServerSocketChannel. A name is the sun_path bytes up to
  // the first NUL (sockaddrToUnixAddressBytes), empty for an unnamed socket.
  const char* uds = "Lsun/nio/ch/UnixDomainSockets;";
  auto uds_name = [](const std::string& name) { return name.substr(0, name.find('\0')); };
  auto uds_path = [](Runtime& rt, Handle path) {
    if (path == kNull) throw_net(rt, "Ljava/net/SocketException;", "Unix domain path not present");
    rt::ArrayObject& a = rt.array(path, "byte[]");
    // MAX_UNIX_DOMAIN_PATH_LEN: room for a NUL at the end and one at the start (abstract names).
    if (a.length > kSunPathMax - 2) throw_net(rt, "Ljava/net/SocketException;", "Unix domain path too long");
    return std::string(reinterpret_cast<const char*>(a.data.data()), a.length) + '\0';
  };
  r.bind(uds, "init", "()Z", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_bool(true); });
  r.bind(uds, "socket0", "()I", [](Runtime& rt, NativeArgs&) -> uint64_t {
    SOCKET h = new_unix_socket(lx::kSockStream);
    if (h == INVALID_SOCKET) throw_socket_error(rt, linux_errno(WSAGetLastError()));
    int32_t fd = -1;
    if (int32_t err = install_fd(std::make_shared<NetSocket>(h, lx::kAfUnix, lx::kSockStream, 0, AF_INET), fd)) {
      throw_socket_error(rt, err);
    }
    return rt::ret_int(fd);
  });
  r.bind(uds, "bind0", "(Ljava/io/FileDescriptor;[B)V", [uds_path](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = -1;
    auto s = nio_socket(rt, a.h(0), fd);
    std::string sun_path = uds_path(rt, a.h(1));
    if (s->family != lx::kAfUnix) throw_socket_error(rt, lx::kEINVAL);
    // An empty path (a single NUL) autobinds, as Linux does for that length.
    if (int32_t err = unix_bind(s, sun_path == std::string(1, '\0') ? std::string() : sun_path)) throw_socket_error(rt, err);
    return 0;
  });
  r.bind(uds, "connect0", "(Ljava/io/FileDescriptor;[B)I", [uds_path](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = -1;
    auto s = nio_socket(rt, a.h(0), fd);
    std::string sun_path = uds_path(rt, a.h(1));
    if (s->family != lx::kAfUnix) throw_socket_error(rt, lx::kEINVAL);
    int32_t err = unix_connect(rt, s, fd, sun_path);
    if (err == kClosedWhileWaiting) return rt::ret_int(kIosInterrupted);
    if (err == lx::kEAGAIN) return rt::ret_int(kIosUnavailable);
    if (err) throw_socket_error(rt, err);
    return rt::ret_int(1);
  });
  r.bind(uds, "accept0", "(Ljava/io/FileDescriptor;Ljava/io/FileDescriptor;[Ljava/lang/Object;)I",
         [uds_name](Runtime& rt, NativeArgs& a) -> uint64_t {
           int32_t fd = -1;
           auto s = nio_socket(rt, a.h(0), fd);
           if (s->family != lx::kAfUnix || !s->unix_listening) throw_io_errno(rt, lx::kEINVAL);
           std::shared_ptr<NetSocket> peer;
           SOCKET c = INVALID_SOCKET;
           int32_t err = 0;
           int64_t n = nio_call(rt, *s, fd, POLLRDNORM, false, err, [&]() -> int64_t {
             c = unix_accept(*s, peer);
             return c == INVALID_SOCKET ? -1 : 0;
           });
           if (n == kIosThrown) throw_io_errno(rt, err);
           if (n < 0) return rt::ret_int(int32_t(n));
           int32_t newfd = -1;
           if (int32_t e = install_fd(unix_accepted(c, *s, *peer), newfd)) throw_io_errno(rt, e);
           set_descriptor(rt, a.h(1), newfd);
           Handle array = a.h(2);
           if (array == kNull) rt.throw_npe({});
           Handle name = unix_bytes(rt, uds_name(peer->unix_name));
           std::memcpy(rt.array(array, "Object[]").data.data(), &name, 4);
           return rt::ret_int(1);
         });
  r.bind(uds, "localAddress0", "(Ljava/io/FileDescriptor;)[B", [uds_name](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = -1;
    auto s = nio_socket(rt, a.h(0), fd);
    return unix_bytes(rt, uds_name(s->unix_name));
  });
  // checkConnect(fd, block, ready): 1 once connected, the connection's error
  // thrown, IOS_UNAVAILABLE while a non-blocking one is pending.
  r.bind("Lsun/nio/ch/SocketChannelImpl;", "checkConnect", "(Ljava/io/FileDescriptor;ZZ)I",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           int32_t fd = fd_of(rt, a.h(0));
           bool block = a.i32(1) != 0, ready = a.i32(2) != 0;
           int16_t revents = 1;
           if (!ready) {
             std::vector<PollEntry> entries(1);
             entries[0].fd = fd;
             entries[0].events = lx::kPollOut;
             int result = poll_wait(rt, entries, block ? -1 : 0, true);
             if (result == kIosInterrupted) throw_io_errno(rt, lx::kEINTR);  // "Poll failed"
             if (!block && result == 0) return rt::ret_int(kIosUnavailable);
             revents = entries[0].revents;
           }
           if (!revents) return 0;
           auto s = socket_of(fd);
           if (!s) throw_socket_error(rt, lx::kEBADF);
           int failed = pending_error(s->handle());
           s->connecting = false;
           if (failed) throw_socket_error(rt, linux_errno(failed));
           return rt::ret_int(1);
         });
  r.bind("Lsun/nio/ch/SocketChannelImpl;", "sendOutOfBandData", "(Ljava/io/FileDescriptor;B)I",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           int32_t fd = -1;
           auto s = nio_socket(rt, a.h(0), fd);
           char byte = char(a.i32(1));
           int32_t err = 0;
           int64_t n = nio_call(rt, *s, fd, POLLWRNORM, true, err, [&]() -> int64_t {
             return send(s->handle(), &byte, 1, MSG_OOB);
           });
           if (n == kIosThrown) throw_io_errno(rt, err);
           return rt::ret_int(int32_t(n));
         });
  r.bind("Lsun/nio/ch/UnixAsynchronousSocketChannelImpl;", "checkConnect", "(I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = socket_of(a.i32(0));
    if (!s) throw_io_errno(rt, lx::kEBADF);  // "getsockopt"
    int failed = pending_error(s->handle());
    s->connecting = false;
    if (failed) throw_socket_error(rt, linux_errno(failed));
    return 0;
  });

  // The selector (PollSelectorImpl): poll(2) over an array of struct pollfd
  // (fd, events, revents: 8 bytes) in native memory; its wakeup writes a byte
  // to its pipe.
  const char* paw = "Lsun/nio/ch/PollArrayWrapper;";
  r.bind(paw, "poll0", "(JIJ)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int64_t address = a.i64(1), timeout = a.i64(4);
    int32_t numfds = a.i32(3);
    if (numfds < 0) throw_io_errno(rt, lx::kEINVAL);
    std::vector<PollEntry> entries(static_cast<size_t>(numfds));
    if (numfds > 0) {
      auto raw = native_span(rt, address, uint64_t(numfds) * 8, false);
      for (size_t i = 0; i < entries.size(); ++i) {
        std::memcpy(&entries[i].fd, raw.data() + i * 8, 4);
        std::memcpy(&entries[i].events, raw.data() + i * 8 + 4, 2);
      }
    }
    int ready = poll_wait(rt, entries, timeout, false);
    if (numfds > 0) {
      auto raw = native_span(rt, address, uint64_t(numfds) * 8, true);  // again: the GIL was released
      for (size_t i = 0; i < entries.size(); ++i) std::memcpy(raw.data() + i * 8 + 6, &entries[i].revents, 2);
    }
    return rt::ret_int(ready);
  });
  r.bind(paw, "interrupt", "(I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    uint8_t byte = 1;
    int32_t err = 0;
    if (once(a.i32(0), std::span<uint8_t>(&byte, 1), true, err) < 0) throw_io_errno(rt, err);
    return 0;
  });

  // IOUtil.
  const char* iou = "Lsun/nio/ch/IOUtil;";
  r.bind(iou, "fdVal", "(Ljava/io/FileDescriptor;)I", [](Runtime& rt, NativeArgs& a) -> uint64_t { return rt::ret_int(fd_of(rt, a.h(0))); });
  r.bind(iou, "setfdVal", "(Ljava/io/FileDescriptor;I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    set_descriptor(rt, a.h(0), a.i32(1));
    return 0;
  });
  r.bind(iou, "configureBlocking", "(Ljava/io/FileDescriptor;Z)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(0));
    bool blocking = a.i32(1) != 0;
    if (fd >= 0 && fd <= 2) return 0;  // the standard streams stay blocking
    try {
      int32_t flags = libcore_vfs().open_flags(fd);
      flags = blocking ? flags & ~vfs::kO_NONBLOCK : flags | vfs::kO_NONBLOCK;
      libcore_vfs().set_status_flags(fd, flags);
      socket_status_flags(fd, flags);
    } catch (const vfs::VfsError& e) {
      throw_io_errno(rt, e.err);
    }
    return 0;
  });
  r.bind(iou, "makePipe", "(Z)J", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fds[2] = {-1, -1};
    if (int32_t err = make_pipe(a.i32(0) == 0, fds)) throw_io_errno(rt, err);
    return rt::ret_long((int64_t(fds[0]) << 32) | int64_t(uint32_t(fds[1])));
  }, S::kPartial, "deux sockets de la boucle locale");
  // drain: reads what the (non-blocking) descriptor holds, 128 bytes at a time.
  r.bind(iou, "drain", "(I)Z", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    uint8_t buf[128];
    int64_t total = 0;
    for (;;) {
      int32_t err = 0;
      int64_t n = once(a.i32(0), std::span<uint8_t>(buf, sizeof buf), false, err);
      total += n;
      if (n < 0 && err != lx::kEAGAIN) throw_io_errno(rt, err);
      if (n == int64_t(sizeof buf)) continue;
      return total > 0 ? 1 : 0;
    }
  });
  r.bind(iou, "fdLimit", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_int(int32_t(vfs::kMaxOpenFiles)); });
  r.bind(iou, "randomBytes", "([B)Z", [](Runtime& rt, NativeArgs&) -> uint64_t {
    throw rt::JavaThrow{construct(rt, "Ljava/lang/UnsupportedOperationException;", "()V", {})};
  });

  // FileDispatcherImpl: preClose0 puts /dev/null under the channel's number
  // (dup2), so that the number stays taken until close0 while
  // NativeThread.signal stops the threads blocked on it.
  const char* fdi = "Lsun/nio/ch/FileDispatcherImpl;";
  r.bind(fdi, "preClose0", "(Ljava/io/FileDescriptor;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(0));
    vfs::Vfs& v = libcore_vfs();
    int32_t null_fd = -1;
    try {
      null_fd = v.open("/dev/null", vfs::kO_RDWR, 0);
    } catch (const vfs::VfsError& e) {
      throw_io_errno(rt, e.err);  // open("/dev/null") failed
    }
    int32_t err = 0;
    try {
      if (fd < 0 || fd > 2) v.dup2(null_fd, fd);  // the host's standard streams stay
    } catch (const vfs::VfsError& e) {
      err = e.err;  // dup2 failed
    }
    v.close(null_fd);
    if (err) throw_io_errno(rt, err);
    return 0;
  });
  r.bind(fdi, "closeIntFD", "(I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = a.i32(0);
    if (fd == -1 || (fd >= 0 && fd <= 2)) return 0;
    try {
      libcore_vfs().close(fd);
    } catch (const vfs::VfsError& e) {
      throw_io_errno(rt, e.err);
    }
    return 0;
  });

  // NativeThread: the running thread's id, and the signal that interrupts its
  // blocking socket call (EINTR, which NIO's natives return as IOS_INTERRUPTED).
  r.bind("Lsun/nio/ch/NativeThread;", "current", "()J",
         [](Runtime& rt, NativeArgs&) -> uint64_t { return rt::ret_long(int64_t(current_thread_id(rt))); });
  r.bind("Lsun/nio/ch/NativeThread;", "signal", "(J)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    int64_t id = a.i64(0);
    std::lock_guard<std::mutex> guard(blocked_lock());
    for (BlockedWait* w : blocked_waits()) {
      if (w->thread != 0 && int64_t(w->thread) == id) w->signal = Wait::kInterrupted;
    }
    return 0;
  });

  // Datagrams.
  const char* dci = "Lsun/nio/ch/DatagramChannelImpl;";
  r.bind(dci, "initIDs", "()V", [](Runtime&, NativeArgs&) -> uint64_t { return 0; });
  // receive0(fd, address, len, connected), instance: the sender kept in
  // `sender`, reused while it sends (cachedSenderInetAddress, cachedSenderPort).
  r.bind(dci, "receive0", "(Ljava/io/FileDescriptor;JIZ)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = -1;
    auto s = nio_socket(rt, a.h(1), fd);
    int32_t len = std::min(a.i32(4), kMaxPacketLen);
    std::span<uint8_t> buf;
    if (len > 0) buf = native_span(rt, a.i64(2), uint64_t(len), true);
    bool connected = a.i32(5) != 0;
    sockaddr_storage ss{};
    int ss_len = sizeof ss;
    int32_t err = 0;
    int64_t n = 0;
    for (;;) {
      n = nio_call(rt, *s, fd, POLLRDNORM, false, err, [&]() -> int64_t {
        ss_len = sizeof ss;
        int got = recvfrom(s->handle(), reinterpret_cast<char*>(buf.data()), int(buf.size()), 0,
                           reinterpret_cast<sockaddr*>(&ss), &ss_len);
        if (got == SOCKET_ERROR && WSAGetLastError() == WSAEMSGSIZE) return int64_t(buf.size());  // cut, as Linux
        return got;
      });
      if (n == kIosThrown && err == lx::kECONNREFUSED && !connected) continue;
      break;
    }
    if (n == kIosThrown) {
      if (err == lx::kECONNREFUSED) throw_port_unreachable(rt);
      throw_socket_error(rt, err);
    }
    if (n < 0) return rt::ret_int(int32_t(n));
    Handle self = a.h(0);
    rt::Class* cls = rt.class_of(self);
    if (n == 0 && ss.ss_family == 0) {  // an orderly shutdown: no sender, receive() returns null
      rt::Field* sender = find_field(cls, "sender", "Ljava/net/SocketAddress;");
      if (sender) rt.object(self)->fields.at(sender->slot) = kNull;
      return 0;
    }
    rt::Field* cached_f = find_field(cls, "cachedSenderInetAddress", "Ljava/net/InetAddress;");
    rt::Field* port_f = find_field(cls, "cachedSenderPort", "I");
    rt::Field* sender_f = find_field(cls, "sender", "Ljava/net/SocketAddress;");
    if (!cached_f || !port_f || !sender_f) rt.throw_new("Ljava/lang/NoSuchFieldError;", "DatagramChannelImpl.sender");
    Handle cached = rt.object(self)->fields.at(cached_f->slot);
    int32_t port = port_of(ss);
    bool same = cached != kNull && int32_t(rt.object(self)->fields.at(port_f->slot)) == port && same_address(rt, ss, cached);
    if (!same) {
      rt::GlobalRef address(rt, to_inet_address(rt, reinterpret_cast<const sockaddr*>(&ss), nullptr));
      Handle isa = construct(rt, kInetSocketAddress, "(Ljava/net/InetAddress;I)V", {address.get(), uint32_t(port)});
      rt.object(self)->fields.at(cached_f->slot) = address.get();
      rt.object(self)->fields.at(port_f->slot) = uint32_t(port);
      rt.object(self)->fields.at(sender_f->slot) = isa;
    }
    return rt::ret_int(int32_t(n));
  });
  r.bind(dci, "send0", "(ZLjava/io/FileDescriptor;JILjava/net/InetAddress;I)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = -1;
    auto s = nio_socket(rt, a.h(2), fd);
    int32_t len = std::min(a.i32(5), kMaxPacketLen);
    std::span<uint8_t> buf;
    if (len > 0) buf = native_span(rt, a.i64(3), uint64_t(len), false);
    Endpoint to = nio_endpoint(rt, *s, a.h(6), a.i32(7), a.i32(1) != 0);
    int32_t err = 0;
    int64_t n = nio_call(rt, *s, fd, POLLWRNORM, true, err, [&]() -> int64_t {
      return sendto(s->handle(), reinterpret_cast<const char*>(buf.data()), int(buf.size()), 0,
                    reinterpret_cast<const sockaddr*>(&to.ss), to.len);
    });
    if (n == kIosThrown) {
      if (err == lx::kECONNREFUSED) throw_port_unreachable(rt);
      throw_socket_error(rt, err);
    }
    return rt::ret_int(int32_t(n));
  });
  // disconnect0: Linux connects to AF_UNSPEC; Windows dissolves the
  // association with the zero address of the socket's family.
  r.bind(dci, "disconnect0", "(Ljava/io/FileDescriptor;Z)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = -1;
    auto s = nio_socket(rt, a.h(0), fd);
    sockaddr_storage ss{};
    ss.ss_family = ADDRESS_FAMILY(s->win_af);
    int len = s->win_af == AF_INET6 ? int(sizeof(sockaddr_in6)) : int(sizeof(sockaddr_in));
    if (connect(s->handle(), reinterpret_cast<const sockaddr*>(&ss), len) == SOCKET_ERROR) {
      throw_socket_error(rt, linux_errno(WSAGetLastError()));
    }
    return 0;
  });
  const char* dd = "Lsun/nio/ch/DatagramDispatcher;";  // connected datagram sockets
  for (bool reading : {true, false}) {
    r.bind(dd, reading ? "read0" : "write0", "(Ljava/io/FileDescriptor;JI)I", [reading](Runtime& rt, NativeArgs& a) -> uint64_t {
      int32_t fd = -1;
      auto s = nio_socket(rt, a.h(0), fd);
      std::span<uint8_t> buf;
      if (a.i32(3) > 0) buf = native_span(rt, a.i64(1), uint64_t(a.i32(3)), reading);
      std::span<uint8_t> one[] = {buf};
      return rt::ret_int(int32_t(nio_io(rt, *s, fd, one, reading)));
    });
    r.bind(dd, reading ? "readv0" : "writev0", "(Ljava/io/FileDescriptor;JI)J", [reading](Runtime& rt, NativeArgs& a) -> uint64_t {
      int32_t fd = -1;
      auto s = nio_socket(rt, a.h(0), fd);
      auto bufs = native_iovecs(rt, a.i64(1), a.i32(3), reading);
      return rt::ret_long(nio_io(rt, *s, fd, bufs, reading));
    });
  }
}

// --- epoll (sun.nio.ch.EPoll, for EPollPort and the asynchronous channels) --------------

constexpr int32_t kEpollCtlAdd = 1, kEpollCtlDel = 2, kEpollCtlMod = 3;
constexpr uint32_t kEpollOneshot = 1u << 30;
constexpr int32_t kENOENT = 2, kEEXIST = 17;

// An epoll instance (epoll_create1): an open file of the Vfs holding its
// interest list. Entries name sockets and pipes (other files are not
// pollable: EPERM) by a weak reference, so that an entry goes away with its
// file's last descriptor, as in Linux. Readiness is level-triggered
// (EPOLLET is taken as level-triggered); EPOLLONESHOT disarms an entry once
// reported. epoll_wait waits in WSAPoll, the GIL released; epoll_ctl wakes
// it through a loopback pair of its own so that it polls the new list.
class EpollFile final : public vfs::OpenFile {
 public:
  struct Entry {
    std::weak_ptr<NetSocket> sock;
    uint32_t events = 0;
    bool armed = true;
  };
  EpollFile(SOCKET wake_read, SOCKET wake_write) : wake_read_(wake_read), wake_write_(wake_write) {}
  ~EpollFile() override {
    closesocket(wake_read_);
    closesocket(wake_write_);
  }
  EpollFile(const EpollFile&) = delete;
  EpollFile& operator=(const EpollFile&) = delete;

  vfs::VStat stat() override {
    vfs::VStat st;
    st.type = vfs::NodeType::kFifo;  // not seekable; an anonymous inode, mode 0600
    st.mode = 0600;
    st.ino = ino_;
    return st;
  }
  size_t read(uint64_t, std::span<uint8_t>) override { throw vfs::VfsError{lx::kEINVAL, "read"}; }
  size_t write(uint64_t, std::span<const uint8_t>) override { throw vfs::VfsError{lx::kEINVAL, "write"}; }
  void truncate(uint64_t) override { throw vfs::VfsError{lx::kEINVAL, "ftruncate"}; }
  bool seekable() const override { return false; }

  // epoll_ctl: 0 or the Linux errno.
  int32_t control(int32_t op, int32_t fd, std::shared_ptr<NetSocket> sock, uint32_t events) {
    std::lock_guard<std::mutex> guard(lock_);
    auto it = entries_.find(fd);
    bool present = it != entries_.end() && !it->second.sock.expired() && it->second.sock.lock() == sock;
    switch (op) {
      case kEpollCtlAdd:
        if (present) return kEEXIST;
        entries_[fd] = Entry{sock, events, true};
        break;
      case kEpollCtlMod:
        if (!present) return kENOENT;
        it->second.events = events;
        it->second.armed = true;
        break;
      case kEpollCtlDel:
        if (!present) return kENOENT;
        entries_.erase(it);
        break;
      default: return lx::kEINVAL;
    }
    char byte = 1;
    send(wake_write_, &byte, 1, 0);  // a waiter polls the new list; a full pipe is woken already
    return 0;
  }

  struct Event {
    uint32_t events;
    int32_t fd;
  };

  // epoll_wait without a timeout (EPoll.epollWait), at most `max` events;
  // run with the GIL released, until `stop` (the runtime shutting down).
  std::vector<Event> wait(size_t max, const std::function<bool()>& stop) {
    std::vector<Event> out;
    while (out.empty() && !stop()) {
      std::vector<std::pair<int32_t, std::shared_ptr<NetSocket>>> polled;
      std::vector<WSAPOLLFD> fds;
      {
        std::lock_guard<std::mutex> guard(lock_);
        for (auto it = entries_.begin(); it != entries_.end();) {
          auto sock = it->second.sock.lock();
          if (!sock) {  // its file was closed
            it = entries_.erase(it);
            continue;
          }
          if (it->second.armed) {
            WSAPOLLFD p{};
            p.fd = sock->handle();
            p.events = to_wsa_events(int16_t(it->second.events & 0xffff));
            fds.push_back(p);
            polled.emplace_back(it->first, std::move(sock));
          }
          ++it;
        }
      }
      WSAPOLLFD wake{};
      wake.fd = wake_read_;
      wake.events = POLLRDNORM;
      fds.push_back(wake);
      if (WSAPoll(fds.data(), ULONG(fds.size()), 50) <= 0) {
        // Not reported by WSAPoll on every Windows version: a failed connect.
        bool failed = false;
        for (size_t i = 0; i < polled.size(); ++i) {
          if (polled[i].second->connecting && pending_error(polled[i].second->handle()) != 0) {
            fds[i].revents = POLLERR | POLLHUP;
            failed = true;
          }
        }
        if (!failed) continue;
      }
      if (fds.back().revents) {
        char drain[64];
        while (recv(wake_read_, drain, sizeof drain, 0) > 0) {
        }
      }
      std::lock_guard<std::mutex> guard(lock_);
      for (size_t i = 0; i < polled.size() && out.size() < max; ++i) {
        auto it = entries_.find(polled[i].first);
        if (it == entries_.end() || !it->second.armed || it->second.sock.lock() != polled[i].second) continue;
        int16_t r = from_wsa_events(fds[i].revents, int16_t(it->second.events & 0xffff));
        if (!r) continue;
        out.push_back(Event{uint32_t(uint16_t(r)), polled[i].first});
        if (it->second.events & kEpollOneshot) it->second.armed = false;
      }
    }
    return out;
  }

 private:
  static uint64_t next_ino() {
    static std::atomic<uint64_t> counter{200000};
    return ++counter;
  }
  std::mutex lock_;
  std::map<int32_t, Entry> entries_;
  SOCKET wake_read_, wake_write_;
  const uint64_t ino_ = next_ino();
};

std::shared_ptr<EpollFile> epoll_of(int32_t epfd, int32_t& err) {
  vfs::Vfs& v = libcore_vfs();
  if (epfd < 3 || !v.is_open(epfd)) {
    err = lx::kEBADF;
    return nullptr;
  }
  auto e = std::dynamic_pointer_cast<EpollFile>(v.open_file(epfd));
  if (!e) err = lx::kEINVAL;
  return e;
}

void register_epoll(rt::NativeRegistry& r) {
  const char* ep = "Lsun/nio/ch/EPoll;";  // static natives
  // struct epoll_event of x86_64 is packed: events (4 bytes), then data (8 bytes).
  r.bind(ep, "eventSize", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_int(12); });
  r.bind(ep, "eventsOffset", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_int(0); });
  r.bind(ep, "dataOffset", "()I", [](Runtime&, NativeArgs&) -> uint64_t { return rt::ret_int(4); });
  r.bind(ep, "epollCreate", "()I", [](Runtime& rt, NativeArgs&) -> uint64_t {
    ensure_winsock();
    auto [one, two] = loopback_pair(SOCK_STREAM);
    if (one == INVALID_SOCKET) throw_io_errno(rt, linux_errno(WSAGetLastError()));  // "epoll_create1 failed"
    auto file = std::make_shared<EpollFile>(one, two);
    int32_t fd = -1;
    try {
      fd = libcore_vfs().install(std::move(file), vfs::kO_RDWR, "anon_inode:[eventpoll]");
    } catch (const vfs::VfsError& e) {
      throw_io_errno(rt, e.err);
    }
    return rt::ret_int(fd);
  }, S::kPartial, "WSAPoll ; EPOLLET traité comme déclenchement par niveau");
  // epollCtl(epfd, op, fd, events): 0 or the errno.
  r.bind(ep, "epollCtl", "(IIII)I", [](Runtime&, NativeArgs& a) -> uint64_t {
    int32_t epfd = a.i32(0), op = a.i32(1), fd = a.i32(2), err = 0;
    auto epoll = epoll_of(epfd, err);
    if (!epoll) return rt::ret_int(err);
    if (fd == epfd) return rt::ret_int(lx::kEINVAL);
    if (fd < 0 || (fd > 2 && !libcore_vfs().is_open(fd))) return rt::ret_int(lx::kEBADF);
    auto sock = socket_of(fd);
    if (!sock) return rt::ret_int(lx::kEPERM);  // regular files and devices are not pollable
    return rt::ret_int(epoll->control(op, fd, std::move(sock), uint32_t(a.i32(3))));
  });
  // epollWait(epfd, address, numfds): waits for ever (RESTARTABLE), the
  // events written at `address`.
  r.bind(ep, "epollWait", "(IJI)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t epfd = a.i32(0), numfds = a.i32(3), err = 0;
    int64_t address = a.i64(1);
    auto epoll = epoll_of(epfd, err);
    if (!epoll) throw_io_errno(rt, err);  // "epoll_wait failed"
    if (numfds <= 0) throw_io_errno(rt, lx::kEINVAL);
    std::vector<EpollFile::Event> events;
    rt.run_blocking([&] { events = epoll->wait(size_t(numfds), [&rt] { return rt.shutting_down(); }); });
    auto raw = native_span(rt, address, uint64_t(events.size()) * 12, true);
    for (size_t i = 0; i < events.size(); ++i) {
      uint64_t data = uint32_t(events[i].fd);
      std::memcpy(raw.data() + i * 12, &events[i].events, 4);
      std::memcpy(raw.data() + i * 12 + 4, &data, 8);
    }
    return rt::ret_int(int32_t(events.size()));
  });

  // EPollPort: its wakeup socketpair (blocking, as socketpair(2) makes it).
  const char* port = "Lsun/nio/ch/EPollPort;";
  r.bind(port, "socketpair", "([I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    Handle sv = a.h(0);
    if (sv == kNull) rt.throw_npe({});
    ensure_winsock();
    auto [one, two] = loopback_pair(SOCK_STREAM);
    if (one == INVALID_SOCKET) throw_io_errno(rt, linux_errno(WSAGetLastError()));  // "socketpair failed"
    int32_t fds[2] = {-1, -1};
    SOCKET handles[2] = {one, two};
    for (int i = 0; i < 2; ++i) {
      auto sock = std::make_shared<NetSocket>(handles[i], lx::kAfUnix, lx::kSockStream, 0, AF_INET);
      if (int32_t err = install_fd(std::move(sock), fds[i])) {
        if (i == 1) libcore_vfs().close(fds[0]);
        else closesocket(two);
        throw_io_errno(rt, err);
      }
    }
    rt::ArrayObject& arr = rt.array(sv, "int[]");
    if (arr.length < 2) rt.throw_new("Ljava/lang/ArrayIndexOutOfBoundsException;", "socketpair");
    std::memcpy(arr.data.data(), fds, sizeof fds);
    return 0;
  }, S::kPartial, "AF_UNIX émulé par deux sockets de la boucle locale");
  r.bind(port, "interrupt", "(I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    uint8_t byte = 1;
    int32_t err = 0;
    if (once(a.i32(0), std::span<uint8_t>(&byte, 1), true, err) < 0) throw_io_errno(rt, err);  // "write failed"
    return 0;
  });
  // drain1: reads one byte, waiting for it (a blocking read).
  r.bind(port, "drain1", "(I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = a.i32(0);
    for (;;) {
      uint8_t byte = 0;
      int32_t err = 0;
      if (once(fd, std::span<uint8_t>(&byte, 1), false, err) >= 0) return 0;
      auto s = socket_of(fd);
      if (err != lx::kEAGAIN || !s) throw_io_errno(rt, err);  // "drain1 failed"
      wait_socket(rt, *s, fd, POLLRDNORM, 0);
    }
  });
  r.bind(port, "close0", "(I)V", [](Runtime&, NativeArgs& a) -> uint64_t {
    int32_t fd = a.i32(0);
    if (fd > 2 && libcore_vfs().is_open(fd)) libcore_vfs().close(fd);
    return 0;
  });
}

}  // namespace

std::optional<int64_t> nio_socket_io(Runtime& rt, int32_t fd, std::span<const std::span<uint8_t>> buffers, bool reading) {
  auto s = socket_of(fd);
  if (!s) return std::nullopt;
  return nio_io(rt, *s, fd, buffers, reading);
}

void register_libcore_net(rt::NativeRegistry& r) {
  const char* lxc = "Llibcore/io/Linux;";  // instance methods: argument 0 is the Linux object
  r.bind(lxc, "socket", "(III)Ljava/io/FileDescriptor;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t domain = a.i32(1), type = a.i32(2), protocol = a.i32(3);
    int32_t base = type & ~(lx::kSockNonblock | lx::kSockCloexec);
    // AF_UNIX (unix_create): no INTERNET permission needed; SOCK_RAW is SOCK_DGRAM.
    if (domain == lx::kAfUnix) {
      if (protocol != 0 && protocol != lx::kAfUnix) throw_errno(rt, "socket", lx::kEPROTONOSUPPORT);
      if (base == lx::kSockRaw) base = lx::kSockDgram;
      if (base != lx::kSockStream && base != lx::kSockDgram && base != lx::kSockSeqpacket) {
        throw_errno(rt, "socket", lx::kESOCKTNOSUPPORT);
      }
      SOCKET h = new_unix_socket(base);
      if (h == INVALID_SOCKET) throw_errno(rt, "socket", linux_errno(WSAGetLastError()));
      auto sock = std::make_shared<NetSocket>(h, lx::kAfUnix, base, 0, AF_INET);
      sock->nonblocking = (type & lx::kSockNonblock) != 0;
      return install_socket(rt, std::move(sock), "socket");
    }
    int af = win_family(domain);
    if (af < 0 || af == AF_UNSPEC) throw_errno(rt, "socket", lx::kEAFNOSUPPORT);  // netlink, packet
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
  }, S::kPartial, "AF_INET et AF_INET6 (TCP, UDP) sur Winsock, AF_UNIX émulé ; ni netlink, ni socket brut");
  // An Internet address given to an AF_UNIX socket: EINVAL (unix_bind and
  // unix_mkname check sun_family).
  r.bind(lxc, "bind", "(Ljava/io/FileDescriptor;Ljava/net/InetAddress;I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = socket_arg(rt, a.h(1), "bind");
    if (s->family == lx::kAfUnix) throw_errno(rt, "bind", lx::kEINVAL);
    do_bind(rt, *s, endpoint_for(rt, *s, a.h(2), a.i32(3), "bind"));
    return 0;
  });
  r.bind(lxc, "bind", "(Ljava/io/FileDescriptor;Ljava/net/SocketAddress;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = socket_arg(rt, a.h(1), "bind");
    if (s->family == lx::kAfUnix) {
      if (a.h(2) == kNull) rt.throw_npe({});
      if (!is_unix_socket_address(rt, a.h(2))) throw_errno(rt, "bind", lx::kEINVAL);
      if (int32_t err = unix_bind(s, unix_sun_path(rt, a.h(2)))) throw_errno(rt, "bind", err);
      return 0;
    }
    do_bind(rt, *s, socket_address_arg(rt, *s, a.h(2), "bind"));
    return 0;
  });
  r.bind(lxc, "listen", "(Ljava/io/FileDescriptor;I)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = socket_arg(rt, a.h(1), "listen");
    if (s->family == lx::kAfUnix) {  // unix_listen: no datagrams, no unbound socket
      if (s->type == lx::kSockDgram) throw_errno(rt, "listen", lx::kEOPNOTSUPP);
      if (s->unix_name.empty()) throw_errno(rt, "listen", lx::kEINVAL);
    }
    if (listen(s->handle(), std::max(a.i32(2), 0)) == SOCKET_ERROR) throw_errno(rt, "listen", linux_errno(WSAGetLastError()));
    s->unix_listening = s->family == lx::kAfUnix;
    return 0;
  });
  // The accepted socket is blocking (accept, not accept4), its peer's
  // address written into the InetSocketAddress given.
  r.bind(lxc, "accept", "(Ljava/io/FileDescriptor;Ljava/net/SocketAddress;)Ljava/io/FileDescriptor;",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto s = socket_arg(rt, a.h(1), "accept");
           if (s->family == lx::kAfUnix) {
             if (!s->unix_listening) throw_errno(rt, "accept", lx::kEINVAL);
             std::shared_ptr<NetSocket> peer;
             SOCKET c = INVALID_SOCKET;
             retry(rt, *s, fd_of(rt, a.h(1)), "accept", POLLRDNORM, s->rcvtimeo_us, false, false, false, [&]() -> int64_t {
               c = unix_accept(*s, peer);
               return c == INVALID_SOCKET ? -1 : 0;
             });
             auto sock = unix_accepted(c, *s, *peer);
             if (Handle address = a.h(2); address != kNull) {
               if (!is_unix_socket_address(rt, address)) {
                 rt.throw_new("Ljava/lang/IllegalArgumentException;", "sockaddrToInetAddress unsupported ss_family: 1");
               }
               fill_unix_address(rt, address, peer->unix_name);
             }
             return install_socket(rt, std::move(sock), "accept");
           }
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
    if (s->family == lx::kAfUnix) throw_errno(rt, "connect", lx::kEINVAL);
    do_connect(rt, *s, fd_of(rt, a.h(1)), endpoint_for(rt, *s, a.h(2), a.i32(3), "connect"));
    return 0;
  });
  r.bind(lxc, "connect", "(Ljava/io/FileDescriptor;Ljava/net/SocketAddress;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = socket_arg(rt, a.h(1), "connect");
    if (s->family == lx::kAfUnix) {
      if (a.h(2) == kNull) rt.throw_npe({});
      if (!is_unix_socket_address(rt, a.h(2))) throw_errno(rt, "connect", lx::kEINVAL);
      int32_t err = unix_connect(rt, s, fd_of(rt, a.h(1)), unix_sun_path(rt, a.h(2)));
      if (err == kClosedWhileWaiting) throw_closed(rt, false, "connect");
      if (err) throw_errno(rt, "connect", err);
      return 0;
    }
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
    WSABUF w = wsabuf(std::span<const uint8_t>(buf.data(), size_t(size)));
    int64_t n = retry(rt, *s, fd_of(rt, a.h(1)), "recvfrom", POLLRDNORM, s->rcvtimeo_us, (flags & lx::kMsgDontwait) != 0, false, false,
                      [&]() -> int64_t {
                        // A datagram longer than the buffer: Linux returns what fits.
                        len = sizeof ss;
                        bool truncated = false;
                        return recv_into(*s, &w, 1, win_msg_flags(flags), from != kNull ? &ss : nullptr,
                                         from != kNull ? &len : nullptr, truncated);
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
    WSABUF w = wsabuf(buf);
    int64_t n = retry(rt, s, fd_of(rt, a.h(1)), "sendto", POLLWRNORM, s.sndtimeo_us, (flags & lx::kMsgDontwait) != 0, false, true,
                      [&]() -> int64_t { return send_from(&rt, s, &w, 1, win_msg_flags(flags), to); });
    return rt::ret_int(int32_t(n));
  };
  r.bind(lxc, "sendtoBytes", "(Ljava/io/FileDescriptor;Ljava/lang/Object;IIILjava/net/InetAddress;I)I",
         [send_to](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto s = socket_arg(rt, a.h(1), "sendto");
           if (a.h(6) == kNull) return send_to(rt, *s, a, nullptr);
           if (s->family == lx::kAfUnix) throw_errno(rt, "sendto", lx::kEINVAL);
           Endpoint to = endpoint_for(rt, *s, a.h(6), a.i32(7), "sendto");
           return send_to(rt, *s, a, &to);
         });
  r.bind(lxc, "sendtoBytes", "(Ljava/io/FileDescriptor;Ljava/lang/Object;IIILjava/net/SocketAddress;)I",
         [send_to](Runtime& rt, NativeArgs& a) -> uint64_t {
           auto s = socket_arg(rt, a.h(1), "sendto");
           if (a.h(6) == kNull) return send_to(rt, *s, a, nullptr);
           Endpoint to = s->family == lx::kAfUnix ? unix_destination(rt, s, a.h(6), "sendto")
                                                  : socket_address_arg(rt, *s, a.h(6), "sendto");
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
    std::vector<int32_t> fds;
    for (const PollEntry& e : entries) fds.push_back(e.fd);
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(timeout, 0));
    int ready = 0;
    for (;;) {
      // Again after a signalled close (the EINTR that makes Linux_poll start
      // over), which may have closed a descriptor or put a marker under its number.
      resolve_poll(entries);
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
            if (rt.shutting_down()) return;
            int slice = 50;
            if (timeout > 0) {  // never before the timeout: the last slice rounded up
              auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now()).count();
              if (left <= 0) return;
              slice = int(std::min<int64_t>(slice, (left + 999) / 1000));
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
    if (type == lx::kSockRaw) type = lx::kSockDgram;
    if (type != lx::kSockStream && type != lx::kSockDgram && type != lx::kSockSeqpacket) {
      throw_errno(rt, "socketpair", lx::kESOCKTNOSUPPORT);
    }
    ensure_winsock();
    auto [one, two] = loopback_pair(type == lx::kSockDgram ? SOCK_DGRAM : SOCK_STREAM);
    if (one == INVALID_SOCKET) throw_errno(rt, "socketpair", linux_errno(WSAGetLastError()));
    auto first = std::make_shared<NetSocket>(one, lx::kAfUnix, type, 0, AF_INET);
    auto second = std::make_shared<NetSocket>(two, lx::kAfUnix, type, 0, AF_INET);
    bool nonblocking = (a.i32(2) & lx::kSockNonblock) != 0;
    for (const auto& sock : {first, second}) {
      unix_attach(sock);  // known ports: their datagrams are the process's own
      sock->nonblocking = nonblocking;
      sock->unix_connected = true;  // unnamed, connected to each other
      int size = kUnixBuffer;
      setsockopt(sock->handle(), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&size), sizeof size);
      setsockopt(sock->handle(), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&size), sizeof size);
    }
    for (auto [java_fd, sock] : {std::pair{a.h(4), first}, std::pair{a.h(5), second}}) {
      Handle made = install_socket(rt, sock, "socketpair");
      rt::Field* f = find_field(rt.class_of(made), "descriptor", "I");
      field_of(rt, java_fd, "Ljava/io/FileDescriptor;", "descriptor", "I") = rt.object(made)->fields.at(f->slot);
    }
    return 0;
  }, S::kPartial, "AF_UNIX émulé par deux sockets TCP (ou UDP) de la boucle locale (sans SCM_RIGHTS)");

  // pipe2(flags): O_NONBLOCK and O_CLOEXEC (no exec here), O_DIRECT ignored.
  r.bind(lxc, "pipe2", "(I)[Ljava/io/FileDescriptor;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t flags = a.i32(1);
    constexpr int32_t kOCloexec = 02000000, kODirect = 040000;
    if (flags & ~(vfs::kO_NONBLOCK | kOCloexec | kODirect)) throw_errno(rt, "pipe2", lx::kEINVAL);
    int32_t fds[2] = {-1, -1};
    if (int32_t err = make_pipe((flags & vfs::kO_NONBLOCK) != 0, fds)) throw_errno(rt, "pipe2", err);
    rt::GlobalRef result(rt, rt.new_array("[Ljava/io/FileDescriptor;", 2));
    for (size_t i = 0; i < 2; ++i) {
      Handle h = new_file_descriptor(rt, fds[i]);
      std::memcpy(&rt.array(result.get(), "FileDescriptor[]").data[i * 4], &h, 4);
    }
    return result.get();
  }, S::kPartial, "deux sockets de la boucle locale");

  // Network interfaces (NetworkInterface): getifaddrs lists each address as
  // bionic does, prepending as it reads the netlink dump (IPv4 addresses,
  // then IPv6 ones, by interface), so that the last address comes first.
  r.bind(lxc, "getifaddrs", "()[Landroid/system/StructIfaddrs;", [](Runtime& rt, NativeArgs&) -> uint64_t {
    struct Entry {
      const Interface* itf;
      const IfAddress* address;
    };
    auto list = interfaces();
    std::vector<Entry> entries;
    for (int family : {AF_INET, AF_INET6}) {
      for (const auto& itf : list) {
        for (const auto& address : itf.addresses) {
          if (address.addr.ss_family == family) entries.push_back(Entry{&itf, &address});
        }
      }
    }
    std::reverse(entries.begin(), entries.end());
    rt::GlobalRef result(rt, rt.new_array("[Landroid/system/StructIfaddrs;", int32_t(entries.size())));
    for (size_t i = 0; i < entries.size(); ++i) {
      const Entry& e = entries[i];
      rt::GlobalRef name(rt, rt.new_string_utf8(e.itf->name));
      rt::GlobalRef addr(rt, to_inet_address(rt, reinterpret_cast<const sockaddr*>(&e.address->addr), nullptr));
      rt::GlobalRef mask(rt, to_inet_address(rt, reinterpret_cast<const sockaddr*>(&e.address->netmask), nullptr));
      rt::GlobalRef broad(rt, e.address->has_broadcast && (e.itf->flags & kIffBroadcast)
                                  ? to_inet_address(rt, reinterpret_cast<const sockaddr*>(&e.address->broadcast), nullptr)
                                  : kNull);
      Handle item = construct(rt, "Landroid/system/StructIfaddrs;",
                              "(Ljava/lang/String;ILjava/net/InetAddress;Ljava/net/InetAddress;Ljava/net/InetAddress;[B)V",
                              {name.get(), e.itf->flags, addr.get(), mask.get(), broad.get(), kNull});
      std::memcpy(&rt.array(result.get(), "StructIfaddrs[]").data[i * 4], &item, 4);
    }
    return result.get();
  }, S::kPartial, "lo et les cartes Wi-Fi/Ethernet actives de l'hôte (wlanN, ethN), sans adresse matérielle");
  r.bind(lxc, "if_nametoindex", "(Ljava/lang/String;)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(1) == kNull) rt.throw_npe({});
    auto itf = interface_named(rt.string_utf8(a.h(1)));
    return rt::ret_int(itf ? itf->index : 0);
  });
  r.bind(lxc, "if_indextoname", "(I)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    for (const auto& itf : interfaces()) {
      if (itf.index == a.i32(1)) return rt.new_string_utf8(itf.name);
    }
    return kNull;
  });
  // ioctl(SIOCGIFFLAGS, SIOCGIFMTU, SIOCGIF*ADDR) on a socket: ENODEV for an
  // unknown interface, ENOTTY on another descriptor.
  auto ioctl_interface = [](Runtime& rt, NativeArgs& a, Handle name) -> Interface {
    std::string n = ifreq_name(rt, name);
    int32_t fd = fd_of(rt, a.h(1));
    if (fd < 0 || (fd > 2 && !libcore_vfs().is_open(fd))) throw_errno(rt, "ioctl", lx::kEBADF);
    auto s = socket_of(fd);
    if (!s || s->pipe != NetSocket::Pipe::kNone) throw_errno(rt, "ioctl", 25);  // ENOTTY
    auto itf = interface_named(n);
    if (!itf) throw_errno(rt, "ioctl", 19);  // ENODEV
    return *itf;
  };
  r.bind(lxc, "ioctlFlags", "(Ljava/io/FileDescriptor;Ljava/lang/String;)I", [ioctl_interface](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(int32_t(ioctl_interface(rt, a, a.h(2)).flags));
  });
  r.bind(lxc, "ioctlMTU", "(Ljava/io/FileDescriptor;Ljava/lang/String;)I", [ioctl_interface](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(ioctl_interface(rt, a, a.h(2)).mtu);
  });
  r.bind(lxc, "ioctlInetAddress", "(Ljava/io/FileDescriptor;ILjava/lang/String;)Ljava/net/InetAddress;",
         [ioctl_interface](Runtime& rt, NativeArgs& a) -> uint64_t {
           Interface itf = ioctl_interface(rt, a, a.h(3));
           constexpr int32_t kSiocgifaddr = 0x8915, kSiocgifdstaddr = 0x8917, kSiocgifbrdaddr = 0x8919,
                             kSiocgifnetmask = 0x891b;
           int32_t cmd = a.i32(2);
           if (cmd != kSiocgifaddr && cmd != kSiocgifdstaddr && cmd != kSiocgifbrdaddr && cmd != kSiocgifnetmask) {
             throw_errno(rt, "ioctl", lx::kEINVAL);
           }
           for (const auto& address : itf.addresses) {  // the first IPv4 address, as devinet_ioctl
             if (address.addr.ss_family != AF_INET) continue;
             sockaddr_storage ss{};
             ss.ss_family = AF_INET;
             if (cmd == kSiocgifaddr || cmd == kSiocgifdstaddr) ss = address.addr;
             else if (cmd == kSiocgifnetmask) ss = address.netmask;
             else if (address.has_broadcast) ss = address.broadcast;
             return to_inet_address(rt, reinterpret_cast<const sockaddr*>(&ss), nullptr);
           }
           throw_errno(rt, "ioctl", lx::kEADDRNOTAVAIL);
         });
  // ioctl(fd, cmd, 0): no request without argument is known here.
  r.bind(lxc, "ioctlRet", "(Ljava/io/FileDescriptor;I)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(1));
    if (fd < 0 || (fd > 2 && !libcore_vfs().is_open(fd))) throw_errno(rt, "ioctl", lx::kEBADF);
    throw_errno(rt, "ioctl", 25);  // ENOTTY
  }, S::kPartial, "aucune requête ioctl sans argument");

  // Options with a structure.
  r.bind(lxc, "getsockoptInAddr", "(Ljava/io/FileDescriptor;II)Ljava/net/InetAddress;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = socket_arg(rt, a.h(1), "getsockopt");
    constexpr int32_t kIpMulticastIf = 32;
    if (a.i32(2) != lx::kIpprotoIp || a.i32(3) != kIpMulticastIf) throw_errno(rt, "getsockopt", lx::kENOPROTOOPT);
    sockaddr_storage ss{};
    auto& sin = reinterpret_cast<sockaddr_in&>(ss);
    sin.sin_family = AF_INET;
    int len = sizeof sin.sin_addr;
    if (getsockopt(s->handle(), IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<char*>(&sin.sin_addr), &len) == SOCKET_ERROR) {
      throw_errno(rt, "getsockopt", linux_errno(WSAGetLastError()));
    }
    return to_inet_address(rt, reinterpret_cast<const sockaddr*>(&ss), nullptr);
  });
  // SO_PEERCRED: this process for its own AF_UNIX sockets; a TCP or UDP
  // socket has no peer credentials (pid 0, uid and gid -1).
  r.bind(lxc, "getsockoptUcred", "(Ljava/io/FileDescriptor;II)Landroid/system/StructUcred;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = socket_arg(rt, a.h(1), "getsockopt");
    constexpr int32_t kSoPeercred = 17;
    if (a.i32(2) != lx::kSolSocket || a.i32(3) != kSoPeercred) throw_errno(rt, "getsockopt", lx::kENOPROTOOPT);
    bool local = s->family == lx::kAfUnix;
    uint32_t pid = local ? uint32_t(GetCurrentProcessId()) : 0, id = local ? uint32_t(kAppUid) : uint32_t(-1);
    return construct(rt, "Landroid/system/StructUcred;", "(III)V", {pid, id, id});
  });
  // SO_BINDTODEVICE needs CAP_NET_RAW, which applications lack.
  r.bind(lxc, "setsockoptIfreq", "(Ljava/io/FileDescriptor;IILjava/lang/String;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    ifreq_name(rt, a.h(4));
    socket_arg(rt, a.h(1), "setsockopt");
    constexpr int32_t kSoBindtodevice = 25;
    throw_errno(rt, "setsockopt", a.i32(2) == lx::kSolSocket && a.i32(3) == kSoBindtodevice ? lx::kEPERM : lx::kENOPROTOOPT);
  });
  // ip_mreqn with only imr_ifindex: IP_MULTICAST_IF by interface index.
  r.bind(lxc, "setsockoptIpMreqn", "(Ljava/io/FileDescriptor;III)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = socket_arg(rt, a.h(1), "setsockopt");
    constexpr int32_t kIpMulticastIf = 32;
    if (a.i32(2) != lx::kIpprotoIp || a.i32(3) != kIpMulticastIf) throw_errno(rt, "setsockopt", lx::kENOPROTOOPT);
    ULONG index = win_interface_index(a.i32(4));
    if (index == ULONG(-1)) throw_errno(rt, "setsockopt", 19);  // ENODEV
    DWORD value = htonl(index);  // 0.0.0.x: an interface index for Windows
    if (setsockopt(s->handle(), IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&value), sizeof value) == SOCKET_ERROR) {
      throw_errno(rt, "setsockopt", linux_errno(WSAGetLastError()));
    }
    return 0;
  });
  // MCAST_JOIN_GROUP and MCAST_LEAVE_GROUP (struct group_req).
  r.bind(lxc, "setsockoptGroupReq", "(Ljava/io/FileDescriptor;IILandroid/system/StructGroupReq;)V", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    auto s = socket_arg(rt, a.h(1), "setsockopt");
    Handle req = a.h(4);
    if (req == kNull) rt.throw_npe({});
    int32_t level = a.i32(2), option = a.i32(3);
    constexpr int32_t kMcastJoinGroup = 42, kMcastLeaveGroup = 45;
    if ((level != lx::kIpprotoIp && level != lx::kIpprotoIpv6) || (option != kMcastJoinGroup && option != kMcastLeaveGroup)) {
      throw_errno(rt, "setsockopt", lx::kENOPROTOOPT);
    }
    GROUP_REQ g{};
    ULONG index = win_interface_index(int32_t(field_of(rt, req, "Landroid/system/StructGroupReq;", "gr_interface", "I")));
    if (index == ULONG(-1)) throw_errno(rt, "setsockopt", 19);  // ENODEV
    g.gr_interface = index;
    Handle group = field_of(rt, req, "Landroid/system/StructGroupReq;", "gr_group", "Ljava/net/InetAddress;");
    Endpoint e = to_sockaddr(rt, group, 0, false);  // verbatim: no IPv4-mapped form
    std::memcpy(&g.gr_group, &e.ss, sizeof g.gr_group);
    int win_level = level == lx::kIpprotoIp ? IPPROTO_IP : IPPROTO_IPV6;
    int win_option = option == kMcastJoinGroup ? MCAST_JOIN_GROUP : MCAST_LEAVE_GROUP;
    if (setsockopt(s->handle(), win_level, win_option, reinterpret_cast<const char*>(&g), sizeof g) == SOCKET_ERROR) {
      throw_errno(rt, "setsockopt", linux_errno(WSAGetLastError()));
    }
    return 0;
  });

  // recvfromNoThrow, recvmsg, recvmsgNoThrow, sendmsg.
  r.bind(lxc, "recvfromNoThrow", "(Ljava/io/FileDescriptor;[BIIILjava/net/InetSocketAddress;)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t fd = fd_of(rt, a.h(1));
    auto buf = extract_io_buffer(rt, a.h(2), a.i32(3), a.i32(4), true);
    int32_t flags = a.i32(5);
    Handle from = a.h(6);
    auto s = socket_of(fd);
    if (!s || s->pipe != NetSocket::Pipe::kNone) {
      return rt::ret_int(fd < 0 || (fd > 2 && !libcore_vfs().is_open(fd)) ? -lx::kEBADF : -lx::kENOTSOCK);
    }
    sockaddr_storage ss{};
    int len = sizeof ss;
    WSABUF w = wsabuf(buf);
    int32_t err = 0;
    int64_t n = retry_nothrow(rt, *s, fd, POLLRDNORM, s->rcvtimeo_us, (flags & lx::kMsgDontwait) != 0, false, err, [&]() -> int64_t {
      len = sizeof ss;
      bool truncated = false;
      return recv_into(*s, &w, 1, win_msg_flags(flags), from != kNull ? &ss : nullptr, from != kNull ? &len : nullptr,
                       truncated);
    });
    if (n < 0) return rt::ret_int(-err);
    if (from != kNull && (ss.ss_family == AF_INET || ss.ss_family == AF_INET6)) {
      fill_inet_socket_address(rt, from, reinterpret_cast<const sockaddr*>(&ss));
    }
    return rt::ret_int(int32_t(n));
  });
  r.bind(lxc, "recvmsg", "(Ljava/io/FileDescriptor;Landroid/system/StructMsghdr;I)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(recv_message(rt, a.h(1), a.h(2), a.i32(3), true));
  }, S::kPartial, "sans message de contrôle");
  r.bind(lxc, "recvmsgNoThrow", "(Ljava/io/FileDescriptor;Landroid/system/StructMsghdr;I)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(recv_message(rt, a.h(1), a.h(2), a.i32(3), false));
  }, S::kPartial, "sans message de contrôle");
  r.bind(lxc, "sendmsg", "(Ljava/io/FileDescriptor;Landroid/system/StructMsghdr;I)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    Handle msg = a.h(2);
    auto bufs = message_buffers(rt, msg, false);
    auto s = socket_arg(rt, a.h(1), "sendmsg");
    Handle control = field_of(rt, msg, "Landroid/system/StructMsghdr;", "msg_control", "[Landroid/system/StructCmsghdr;");
    if (control != kNull && rt.array(control, "StructCmsghdr[]").length > 0) {
      throw_errno(rt, "sendmsg", lx::kEOPNOTSUPP);  // no ancillary data (SCM_RIGHTS, IP_PKTINFO...) here
    }
    Handle name = field_of(rt, msg, "Landroid/system/StructMsghdr;", "msg_name", "Ljava/net/SocketAddress;");
    Endpoint to;
    if (name != kNull) {
      to = s->family == lx::kAfUnix ? unix_destination(rt, s, name, "sendmsg") : socket_address_arg(rt, *s, name, "sendmsg");
    }
    int32_t flags = a.i32(3);
    int32_t fd = fd_of(rt, a.h(1));
    int64_t n = retry(rt, *s, fd, "sendmsg", POLLWRNORM, s->sndtimeo_us, (flags & lx::kMsgDontwait) != 0, false, true,
                      [&]() -> int64_t {
                        return send_from(&rt, *s, bufs.data(), DWORD(bufs.size()), win_msg_flags(flags),
                                         name != kNull ? &to : nullptr);
                      });
    return rt::ret_int(int32_t(n));
  }, S::kPartial, "sans message de contrôle");

  register_nio_net(r);
  register_epoll(r);

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
               case Wait::kReady:
               case Wait::kInterrupted: break;
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
