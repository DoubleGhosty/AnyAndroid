package android.system;

// Declaration of the framework class for javac only (see ../../../README.md):
// no constant initializer, so that javac never inlines a value.
public final class StructPasswd {
  public final String pw_name;
  public final int pw_uid;
  public final String pw_dir;
  public final String pw_shell;

  public StructPasswd(String pw_name, int pw_uid, int pw_gid, String pw_dir, String pw_shell) {
    this.pw_name = pw_name;
    this.pw_uid = pw_uid;
    this.pw_dir = pw_dir;
    this.pw_shell = pw_shell;
    throw new RuntimeException("stub");
  }
}
