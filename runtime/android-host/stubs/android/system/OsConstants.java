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
}

final class Stubs {
  static int integer() { throw new RuntimeException("stub"); }
}
