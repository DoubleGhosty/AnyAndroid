package android.system;

// Declaration of the framework class for javac only (see ../../../README.md):
// no constant initializer, so that javac never inlines a value.
public final class ErrnoException extends Exception {
  public ErrnoException(String functionName, int errno) {
    throw new RuntimeException("stub");
  }
}
