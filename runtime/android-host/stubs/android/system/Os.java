package android.system;

import java.io.FileDescriptor;

// Declaration of the framework class for javac only (see ../../../README.md):
// no constant initializer, so that javac never inlines a value.
public final class Os {
  public static FileDescriptor open(String path, int flags, int mode) throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static void close(FileDescriptor fd) throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static void symlink(String oldPath, String newPath) throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static String readlink(String path) throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static int readv(FileDescriptor fd, Object[] buffers, int[] offsets, int[] byteCounts)
      throws ErrnoException, java.io.InterruptedIOException {
    throw new RuntimeException("stub");
  }

  public static int writev(FileDescriptor fd, Object[] buffers, int[] offsets, int[] byteCounts)
      throws ErrnoException, java.io.InterruptedIOException {
    throw new RuntimeException("stub");
  }

  public static long lseek(FileDescriptor fd, long offset, int whence) throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static FileDescriptor socket(int domain, int type, int protocol) throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static FileDescriptor[] pipe2(int flags) throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static int read(FileDescriptor fd, byte[] bytes, int byteOffset, int byteCount)
      throws ErrnoException, java.io.InterruptedIOException {
    throw new RuntimeException("stub");
  }

  public static int write(FileDescriptor fd, byte[] bytes, int byteOffset, int byteCount)
      throws ErrnoException, java.io.InterruptedIOException {
    throw new RuntimeException("stub");
  }

  public static StructPasswd getpwuid(int uid) throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static StructPasswd getpwnam(String name) throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static StructRlimit getrlimit(int resource) throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static void setuid(int uid) throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static StructCapUserData[] capget(StructCapUserHeader hdr) throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static String strsignal(int signal) {
    throw new RuntimeException("stub");
  }

  public static int waitpid(int pid, Int32Ref status, int options) throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static FileDescriptor memfd_create(String name, int flags) throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static long mmap(long address, long byteCount, int prot, int flags, FileDescriptor fd, long offset)
      throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static void munmap(long address, long byteCount) throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static void mincore(long address, long byteCount, byte[] vector) throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static void msync(long address, long byteCount, int flags) throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static long sendfile(FileDescriptor outFd, FileDescriptor inFd, Int64Ref offset, long byteCount)
      throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static int readNoThrow(FileDescriptor fd, byte[] bytes, int byteOffset, int byteCount) {
    throw new RuntimeException("stub");
  }

  public static StructIfaddrs[] getifaddrs() throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static int if_nametoindex(String name) {
    throw new RuntimeException("stub");
  }

  public static int recvmsg(FileDescriptor fd, StructMsghdr msg, int flags)
      throws ErrnoException, java.net.SocketException {
    throw new RuntimeException("stub");
  }

  public static int sendmsg(FileDescriptor fd, StructMsghdr msg, int flags)
      throws ErrnoException, java.net.SocketException {
    throw new RuntimeException("stub");
  }

  public static void bind(FileDescriptor fd, java.net.InetAddress address, int port)
      throws ErrnoException, java.net.SocketException {
    throw new RuntimeException("stub");
  }

  public static java.net.SocketAddress getsockname(FileDescriptor fd) throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static FileDescriptor dup(FileDescriptor oldFd) throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static FileDescriptor dup2(FileDescriptor oldFd, int newFd) throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static int fcntlInt(FileDescriptor fd, int cmd, int arg) throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static int ioctlInt(FileDescriptor fd, int cmd) throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static int getpgid(int pid) throws ErrnoException {
    throw new RuntimeException("stub");
  }

  public static int getuid() {
    throw new RuntimeException("stub");
  }

  public static int getpid() {
    throw new RuntimeException("stub");
  }

  public static int umask(int mask) {
    throw new RuntimeException("stub");
  }

  public static long sysconf(int name) {
    throw new RuntimeException("stub");
  }
}
