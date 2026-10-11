package aa.host;

import aa.host.services.PackageService;
import aa.host.services.PermissionService;
import aa.host.services.SystemFeatures;
import android.content.pm.SystemFeaturesCache;
import android.os.ServiceManager;
import com.android.internal.os.ApplicationSharedMemory;

/**
 * AnyAndroid's stand-in for system_server (ADR-006): registers the local system services in the
 * process's service manager. The CLI calls {@link #start} once libcore is up, with the path of
 * the application's APK in the virtual file system.
 */
public final class SystemServer {
  /** The application's uid: the one Os.getuid() reports (u0_a0). */
  public static final int APP_UID = 10000;

  private SystemServer() {}

  public static void start(String apkPath) {
    // What ApplicationThread.bindApplication installs before anything else
    // (the shared memory is the one system_server would have created).
    ApplicationSharedMemory.setInstance(ApplicationSharedMemory.create());
    SystemFeaturesCache.setInstance(new SystemFeaturesCache(SystemFeatures.list()));
    PackageService packages = new PackageService(apkPath);
    ServiceManager.addService("package", packages);
    ServiceManager.addService("permissionmgr", new PermissionService(packages));
  }
}
