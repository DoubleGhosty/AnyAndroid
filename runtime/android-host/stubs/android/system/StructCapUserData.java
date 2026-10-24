package android.system;

// Declaration of the framework class for javac only (see ../../../README.md):
// no constant initializer, so that javac never inlines a value.
public final class StructCapUserData {
  public final int effective;

  public StructCapUserData(int effective, int permitted, int inheritable) {
    this.effective = effective;
    throw new RuntimeException("stub");
  }
}
