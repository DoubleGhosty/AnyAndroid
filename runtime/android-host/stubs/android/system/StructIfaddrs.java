package android.system;

import java.net.InetAddress;

// Declaration of the framework class for javac only (see ../../../README.md):
// no constant initializer, so that javac never inlines a value.
public final class StructIfaddrs {
  public final String ifa_name;
  public final int ifa_flags;
  public final InetAddress ifa_addr;
  public final InetAddress ifa_netmask;
  public final InetAddress ifa_broadaddr;
  public final byte[] hwaddr;

  public StructIfaddrs(String ifa_name, int ifa_flags, InetAddress ifa_addr, InetAddress ifa_netmask,
      InetAddress ifa_broadaddr, byte[] hwaddr) {
    this.ifa_name = ifa_name;
    this.ifa_flags = ifa_flags;
    this.ifa_addr = ifa_addr;
    this.ifa_netmask = ifa_netmask;
    this.ifa_broadaddr = ifa_broadaddr;
    this.hwaddr = hwaddr;
    throw new RuntimeException("stub");
  }
}
