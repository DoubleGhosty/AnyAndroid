package android.system;

import java.net.SocketAddress;
import java.nio.ByteBuffer;

// Declaration of the framework class for javac only (see ../../../README.md):
// no constant initializer, so that javac never inlines a value.
public final class StructMsghdr {
  public SocketAddress msg_name;
  public int msg_flags;

  public StructMsghdr(SocketAddress msg_name, ByteBuffer[] msg_iov, StructCmsghdr[] msg_control, int msg_flags) {
    throw new RuntimeException("stub");
  }
}
