package aa.host.services;

import android.app.ContentProviderHolder;
import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.content.pm.ApplicationInfo;
import android.content.pm.ProviderInfo;

/**
 * The "activity" service (ADR-006 J4): the activity manager, as far as the application needs
 * it. Content providers of the system come from here: "settings" is {@link SettingsProvider};
 * any other authority is unknown (null, as for a provider that is not installed).
 */
public final class ActivityService extends IActivityManager.Stub {
  /** Process.SYSTEM_UID: the providers of the system run as system. */
  private static final int SYSTEM_UID = 1000;

  private final PackageService packages;
  private ContentProviderHolder settings;

  public ActivityService(PackageService packages) {
    super(LocalPermissionEnforcer.INSTANCE);
    this.packages = packages;
  }

  /** ActivityManagerService.checkComponentPermission: root and system hold every permission. */
  @Override
  public int checkPermissionForDevice(String permission, int pid, int uid, int deviceId) {
    if (uid == 0 || uid == SYSTEM_UID) return 0;  // PERMISSION_GRANTED
    return packages.checkUidPermission(permission, uid);
  }

  @Override
  public synchronized ContentProviderHolder getContentProvider(
      IApplicationThread caller, String callingPackage, String name, int userId, boolean stable) {
    if (!SettingsProvider.AUTHORITY.equals(name)) return null;
    if (settings == null) {
      ProviderInfo info = new ProviderInfo();
      info.authority = SettingsProvider.AUTHORITY;
      info.packageName = "com.android.providers.settings";
      info.name = info.packageName + ".SettingsProvider";
      info.exported = true;
      info.applicationInfo = new ApplicationInfo();
      info.applicationInfo.packageName = info.packageName;
      info.applicationInfo.uid = SYSTEM_UID;
      ContentProviderHolder holder = new ContentProviderHolder(info);
      SettingsProvider provider = new SettingsProvider();
      provider.attachInfo(null, info);  // as ActivityThread.installProvider: authority, then onCreate
      holder.provider = provider.getIContentProvider();
      holder.noReleaseNeeded = true;  // never unbound: no reference counting with this service
      settings = holder;
    }
    return settings;
  }
}
