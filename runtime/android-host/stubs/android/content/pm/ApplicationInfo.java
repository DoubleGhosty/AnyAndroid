package android.content.pm;

public class ApplicationInfo extends PackageItemInfo {
  public String sourceDir;
  public String publicSourceDir;
  public String dataDir;
  public String nativeLibraryDir;
  public String processName;
  public int uid;
  public int targetSdkVersion;
  public int flags;

  public void initForUser(int userId) {}
}
