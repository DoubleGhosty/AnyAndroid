package aa.host.probe;

// Targets of LibcoreProbe's reflection access checks, in a package other than
// the probe's.
public class Hidden {
  protected static int level = 3;
  protected int depth = 5;
  int packageCount = 4;

  protected Hidden() {}

  private static String secret() {
    return "secret";
  }
}

class PackagePrivate {
  public PackagePrivate() {}
}
