package aa.host;

import aa.host.services.AccessibilityService;
import aa.host.services.ActivityService;
import aa.host.services.ActivityTaskService;
import aa.host.services.AudioService;
import aa.host.services.DisplayService;
import aa.host.services.InputMethodService;
import aa.host.services.InputService;
import aa.host.services.NotificationService;
import aa.host.services.PackageService;
import aa.host.services.PermissionService;
import aa.host.services.SystemFeatures;
import aa.host.services.WindowService;
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
    ApplicationSharedMemory memory = ApplicationSharedMemory.create();
    ApplicationSharedMemory.setInstance(memory);
    SystemFeaturesCache features = new SystemFeaturesCache(SystemFeatures.list());
    SystemFeaturesCache.setInstance(features);
    // What bindApplication reads back from the shared memory (SystemServer.startOtherServices).
    memory.writeSystemFeaturesCache(features.getSdkFeatureVersions());
    PackageService packages = new PackageService(apkPath);
    ServiceManager.addService("package", packages);
    ServiceManager.addService("permissionmgr", new PermissionService(packages));
    ServiceManager.addService("window", new WindowService());
    ServiceManager.addService("display", new DisplayService());
    ServiceManager.addService("input", new InputService());
    ServiceManager.addService("input_method", new InputMethodService());
    ServiceManager.addService("accessibility", new AccessibilityService());
    ServiceManager.addService("audio", new AudioService());
    ServiceManager.addService("notification", new NotificationService());
    ServiceManager.addService("activity", new ActivityService(packages));
    ServiceManager.addService("activity_task", new ActivityTaskService());
  }
}
