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

  public static int getpid() {
    throw new RuntimeException("stub");
  }

  public static int umask(int mask) {
    throw new RuntimeException("stub");
  }
}
