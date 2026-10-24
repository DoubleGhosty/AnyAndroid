package android.system;

// Declaration of the framework class for javac only (see ../../../README.md):
// no constant initializer, so that javac never inlines a value.
public final class StructRlimit {
  public final long rlim_cur;
  public final long rlim_max;

  public StructRlimit(long rlim_cur, long rlim_max) {
    this.rlim_cur = rlim_cur;
    this.rlim_max = rlim_max;
    throw new RuntimeException("stub");
  }
}
