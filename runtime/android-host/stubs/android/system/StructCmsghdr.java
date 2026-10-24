package android.system;

// Declaration of the framework class for javac only (see ../../../README.md):
// no constant initializer, so that javac never inlines a value.
public final class StructCmsghdr {
  public final int cmsg_level;
  public final int cmsg_type;
  public final byte[] cmsg_data;

  public StructCmsghdr(int cmsg_level, int cmsg_type, byte[] value) {
    this.cmsg_level = cmsg_level;
    this.cmsg_type = cmsg_type;
    this.cmsg_data = value;
    throw new RuntimeException("stub");
  }
}
