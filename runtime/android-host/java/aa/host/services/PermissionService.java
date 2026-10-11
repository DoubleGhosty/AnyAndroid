package aa.host.services;

import android.permission.IPermissionManager;
import java.util.ArrayList;
import java.util.List;

/**
 * The "permissionmgr" service (ADR-006 J3). Permission state is the package service's: the
 * permissions the manifest requests are granted. No split permissions are declared (the device
 * configuration's split-permission list is not read yet).
 */
public final class PermissionService extends IPermissionManager.Stub {
  private final PackageService packages;

  public PermissionService(PackageService packages) {
    super(LocalPermissionEnforcer.INSTANCE);
    this.packages = packages;
  }

  @Override
  public List getSplitPermissions(boolean upgrade) {
    return new ArrayList<>();
  }

  @Override
  public int checkPermission(String packageName, String permissionName, String persistentDeviceId, int userId) {
    return packages.checkPermission(permissionName, packageName, userId);
  }

  @Override
  public int checkUidPermission(int uid, String permissionName, int deviceId) {
    return packages.checkUidPermission(permissionName, uid);
  }
}
