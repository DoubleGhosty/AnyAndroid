package android.os;

// Declaration of the framework class for javac only (see ../../../README.md):
// no constant initializer, so that javac never inlines a value.
public class Build {
  public static final String MANUFACTURER = Stubs.string();

  public static class VERSION {
    public static final int SDK_INT = Stubs.integer();
  }
}

final class Stubs {
  static String string() { throw new RuntimeException("stub"); }
  static int integer() { throw new RuntimeException("stub"); }
}
