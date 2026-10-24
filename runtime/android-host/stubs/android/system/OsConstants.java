package android.system;

// Declaration of the framework class for javac only (see ../../../README.md):
// no constant initializer, so that javac never inlines a value.
public final class OsConstants {
  public static final int O_RDWR = Stubs.integer();
  public static final int O_CREAT = Stubs.integer();
  public static final int O_TRUNC = Stubs.integer();
  public static final int SEEK_SET = Stubs.integer();
  public static final int F_GETFD = Stubs.integer();
  public static final int FD_CLOEXEC = Stubs.integer();
  public static final int FIONREAD = Stubs.integer();
  public static final int O_NOFOLLOW = Stubs.integer();
  public static final int AI_ADDRCONFIG = Stubs.integer();
  public static final int EAI_NONAME = Stubs.integer();
  public static final int SIGRTMIN = Stubs.integer();
  public static final int _SC_PAGESIZE = Stubs.integer();
  public static final int AF_INET = Stubs.integer();
  public static final int SOCK_STREAM = Stubs.integer();
  public static final int O_NONBLOCK = Stubs.integer();
  public static final int EAGAIN = Stubs.integer();
  public static final int RLIMIT_NOFILE = Stubs.integer();
  public static final int PROT_READ = Stubs.integer();
  public static final int MAP_SHARED = Stubs.integer();
  public static final int MS_SYNC = Stubs.integer();
  public static final int SOCK_DGRAM = Stubs.integer();
  public static final int MSG_TRUNC = Stubs.integer();
}

final class Stubs {
  static int integer() { throw new RuntimeException("stub"); }
}
