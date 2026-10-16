package android.app;

import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;

public final class ActivityThread {
  public static IPackageManager getPackageManager() {
    return null;
  }

  public static ActivityThread currentActivityThread() {
    return null;
  }

  public static Application currentApplication() {
    return null;
  }

  public static void main(String[] args) {}

  public LoadedApk getPackageInfoNoCheck(ApplicationInfo ai) {
    return null;
  }
}
