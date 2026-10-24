package android.system;

// Declaration of the framework class for javac only (see ../../../README.md):
// no constant initializer, so that javac never inlines a value.
public final class ErrnoException extends Exception {
  public final int errno;

  public ErrnoException(String functionName, int errno) {
    this.errno = errno;
    throw new RuntimeException("stub");
  }
}
