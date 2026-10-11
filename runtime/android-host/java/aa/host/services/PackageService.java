package aa.host.services;

import aa.host.SystemServer;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInfoList;
import android.content.pm.PackageParser;
import android.content.pm.ParceledListSlice;
import android.content.pm.ProviderInfo;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.content.pm.pkg.FrameworkPackageUserState;
import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * The "package" service (ADR-006 J3): the one installed application, parsed from its APK by the
 * framework's own PackageParser on first use. Other packages are unknown (null, -1).
 */
public final class PackageService extends IPackageManager.Stub {
  private static final int PERMISSION_GRANTED = 0;
  private static final int PERMISSION_DENIED = -1;

  private final String apkPath;
  private PackageParser.Package pkg;

  public PackageService(String apkPath) {
    super(LocalPermissionEnforcer.INSTANCE);
    this.apkPath = apkPath;
  }

  private synchronized PackageParser.Package pkg() {
    if (pkg == null) {
      try {
        PackageParser.Package p = new PackageParser().parsePackage(new File(apkPath), 0);
        // What PackageManagerService completes after parsing: uid, paths,
        // process name, data directories of user 0.
        ApplicationInfo ai = p.applicationInfo;
        ai.uid = SystemServer.APP_UID;
        ai.sourceDir = apkPath;
        ai.publicSourceDir = apkPath;
        ai.nativeLibraryDir = new File(new File(apkPath).getParentFile(), "lib/x86_64").getPath();
        if (ai.processName == null) ai.processName = p.packageName;
        ai.initForUser(0);
        pkg = p;
      } catch (PackageParser.PackageParserException e) {
        throw new IllegalStateException("[AnyAndroid] APK illisible par PackageParser : " + apkPath, e);
      }
    }
    return pkg;
  }

  private boolean ours(String packageName) {
    return packageName != null && packageName.equals(pkg().packageName);
  }

  private static <C extends PackageParser.Component<?>> C find(List<C> components, ComponentName name) {
    if (name == null || components == null) return null;
    for (C c : components) {
      if (c.className.equals(name.getClassName())) return c;
    }
    return null;
  }

  @Override
  public ApplicationInfo getApplicationInfo(String packageName, long flags, int userId) {
    if (!ours(packageName)) return null;
    return PackageParser.generateApplicationInfo(pkg(), (int) flags, FrameworkPackageUserState.DEFAULT, userId);
  }

  @Override
  public PackageInfo getPackageInfo(String packageName, long flags, int userId) {
    if (!ours(packageName)) return null;
    return PackageParser.generatePackageInfo(
        pkg(), null, (int) flags, 0L, 0L, Collections.emptySet(), FrameworkPackageUserState.DEFAULT, userId);
  }

  @Override
  public ActivityInfo getActivityInfo(ComponentName className, long flags, int userId) {
    if (className == null || !ours(className.getPackageName())) return null;
    PackageParser.Activity a = find(pkg().activities, className);
    return a == null ? null : PackageParser.generateActivityInfo(a, (int) flags, FrameworkPackageUserState.DEFAULT, userId);
  }

  @Override
  public ActivityInfo getReceiverInfo(ComponentName className, long flags, int userId) {
    if (className == null || !ours(className.getPackageName())) return null;
    PackageParser.Activity a = find(pkg().receivers, className);
    return a == null ? null : PackageParser.generateActivityInfo(a, (int) flags, FrameworkPackageUserState.DEFAULT, userId);
  }

  @Override
  public ServiceInfo getServiceInfo(ComponentName className, long flags, int userId) {
    if (className == null || !ours(className.getPackageName())) return null;
    PackageParser.Service s = find(pkg().services, className);
    return s == null ? null : PackageParser.generateServiceInfo(s, (int) flags, FrameworkPackageUserState.DEFAULT, userId);
  }

  @Override
  public ProviderInfo getProviderInfo(ComponentName className, long flags, int userId) {
    if (className == null || !ours(className.getPackageName())) return null;
    PackageParser.Provider p = find(pkg().providers, className);
    return p == null ? null : PackageParser.generateProviderInfo(p, (int) flags, FrameworkPackageUserState.DEFAULT, userId);
  }

  /** Activities of the application matching the intent: by component, else by intent filter. */
  private List<ResolveInfo> activitiesFor(Intent intent, String resolvedType, long flags, int userId) {
    List<ResolveInfo> out = new ArrayList<>();
    if (intent == null) return out;
    ComponentName component = intent.getComponent();
    if (component != null) {
      ActivityInfo info = getActivityInfo(component, flags, userId);
      if (info != null) {
        ResolveInfo ri = new ResolveInfo();
        ri.activityInfo = info;
        out.add(ri);
      }
      return out;
    }
    if (intent.getPackage() != null && !ours(intent.getPackage())) return out;
    String type = resolvedType != null ? resolvedType : intent.getType();
    for (PackageParser.Activity a : pkg().activities) {
      if (a.intents == null) continue;
      for (PackageParser.ActivityIntentInfo filter : a.intents) {
        int match = filter.match(
            intent.getAction(), type, intent.getScheme(), intent.getData(), intent.getCategories(), "AnyAndroid");
        if (match < 0) continue;
        ResolveInfo ri = new ResolveInfo();
        ri.activityInfo = PackageParser.generateActivityInfo(a, (int) flags, FrameworkPackageUserState.DEFAULT, userId);
        ri.filter = filter;
        ri.match = match;
        out.add(ri);
        break;
      }
    }
    return out;
  }

  @Override
  public ResolveInfo resolveIntent(Intent intent, String resolvedType, long flags, int userId) {
    List<ResolveInfo> found = activitiesFor(intent, resolvedType, flags, userId);
    return found.isEmpty() ? null : found.get(0);
  }

  @Override
  public ParceledListSlice queryIntentActivities(Intent intent, String resolvedType, long flags, int userId) {
    return new ParceledListSlice<>(activitiesFor(intent, resolvedType, flags, userId));
  }

  @Override
  public PackageInfoList getInstalledPackages(long flags, int userId) {
    List<PackageInfo> list = new ArrayList<>();
    list.add(getPackageInfo(pkg().packageName, flags, userId));
    return new PackageInfoList(list);
  }

  @Override
  public int getPackageUid(String packageName, long flags, int userId) {
    return ours(packageName) ? SystemServer.APP_UID : -1;
  }

  @Override
  public String[] getPackagesForUid(int uid) {
    return uid == SystemServer.APP_UID ? new String[] {pkg().packageName} : null;
  }

  @Override
  public String getNameForUid(int uid) {
    return uid == SystemServer.APP_UID ? pkg().packageName : null;
  }

  /** Permissions the manifest requests are granted; AnyAndroid has no permission prompt yet. */
  @Override
  public int checkPermission(String permName, String pkgName, int userId) {
    if (!ours(pkgName) || pkg().requestedPermissions == null) return PERMISSION_DENIED;
    return pkg().requestedPermissions.contains(permName) ? PERMISSION_GRANTED : PERMISSION_DENIED;
  }

  @Override
  public int checkUidPermission(String permName, int uid) {
    return uid == SystemServer.APP_UID ? checkPermission(permName, pkg().packageName, 0) : PERMISSION_DENIED;
  }

  @Override
  public boolean hasSystemFeature(String name, int version) {
    return false;
  }

  @Override
  public ParceledListSlice getSystemAvailableFeatures() {
    return ParceledListSlice.emptyList();
  }

  @Override
  public int getTargetSdkVersion(String packageName) {
    return ours(packageName) ? pkg().applicationInfo.targetSdkVersion : -1;
  }

  @Override
  public boolean isPackageAvailable(String packageName, int userId) {
    return ours(packageName);
  }

  @Override
  public int getComponentEnabledSetting(ComponentName componentName, int userId) {
    return 0;  // COMPONENT_ENABLED_STATE_DEFAULT
  }

  @Override
  public int getApplicationEnabledSetting(String packageName, int userId) {
    return 0;  // COMPONENT_ENABLED_STATE_DEFAULT
  }

  @Override
  public boolean isSafeMode() {
    return false;
  }

  @Override
  public boolean isInstantApp(String packageName, int userId) {
    return false;
  }
}
