package android.system;

// Declaration of the framework class for javac only (see ../../../README.md).
public final class UnixSocketAddress extends java.net.SocketAddress {
  private UnixSocketAddress(byte[] sun_path) {
    throw new RuntimeException("stub");
  }

  public static UnixSocketAddress createAbstract(String name) {
    throw new RuntimeException("stub");
  }

  public static UnixSocketAddress createFileSystem(String pathName) {
    throw new RuntimeException("stub");
  }

  public static UnixSocketAddress createUnnamed() {
    throw new RuntimeException("stub");
  }

  public byte[] getSunPath() {
    throw new RuntimeException("stub");
  }
}
