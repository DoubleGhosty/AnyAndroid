package android.content.pm.pkg;

// Declaration for javac only: DEFAULT is not a constant expression, so javac
// reads the framework's field at run time.
public interface FrameworkPackageUserState {
  FrameworkPackageUserState DEFAULT = Stub.state();

  final class Stub {
    static FrameworkPackageUserState state() {
      throw new RuntimeException("stub");
    }
  }
}
