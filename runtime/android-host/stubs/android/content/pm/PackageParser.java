package android.content.pm;

import android.content.ComponentName;
import android.content.IntentFilter;
import android.content.pm.pkg.FrameworkPackageUserState;
import java.io.File;
import java.util.ArrayList;
import java.util.Set;

public class PackageParser {
  public PackageParser() {}

  public Package parsePackage(File packageFile, int flags) throws PackageParserException {
    return null;
  }

  public static ApplicationInfo generateApplicationInfo(
      Package p, int flags, FrameworkPackageUserState state, int userId) {
    return null;
  }

  public static PackageInfo generatePackageInfo(Package p, int[] gids, int flags, long firstInstallTime,
      long lastUpdateTime, Set<String> grantedPermissions, FrameworkPackageUserState state, int userId) {
    return null;
  }

  public static ActivityInfo generateActivityInfo(
      Activity a, int flags, FrameworkPackageUserState state, int userId) {
    return null;
  }

  public static ServiceInfo generateServiceInfo(
      Service s, int flags, FrameworkPackageUserState state, int userId) {
    return null;
  }

  public static ProviderInfo generateProviderInfo(
      Provider p, int flags, FrameworkPackageUserState state, int userId) {
    return null;
  }

  public static class PackageParserException extends Exception {}

  public static final class Package {
    public String packageName;
    public ApplicationInfo applicationInfo;
    public ArrayList<Activity> activities;
    public ArrayList<Activity> receivers;
    public ArrayList<Service> services;
    public ArrayList<Provider> providers;
    public ArrayList<String> requestedPermissions;
  }

  public abstract static class Component<II extends IntentInfo> {
    public String className;
    public ArrayList<II> intents;

    public ComponentName getComponentName() {
      return null;
    }
  }

  public abstract static class IntentInfo extends IntentFilter {}

  public static final class ActivityIntentInfo extends IntentInfo {}

  public static final class ServiceIntentInfo extends IntentInfo {}

  public static final class ProviderIntentInfo extends IntentInfo {}

  public static final class Activity extends Component<ActivityIntentInfo> {
    public ActivityInfo info;
  }

  public static final class Service extends Component<ServiceIntentInfo> {
    public ServiceInfo info;
  }

  public static final class Provider extends Component<ProviderIntentInfo> {
    public ProviderInfo info;
  }
}
