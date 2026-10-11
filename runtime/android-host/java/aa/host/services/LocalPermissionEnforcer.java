package aa.host.services;

import android.os.PermissionEnforcer;

/**
 * Given to the AIDL stubs that take a PermissionEnforcer (Stub(PermissionEnforcer)), instead of
 * the one their default constructor fetches from the system context, which this process does not
 * have. Only a service implementation calls the enforcer's checks, and AnyAndroid's services
 * call none: the application's permissions are answered by the "package" service.
 */
final class LocalPermissionEnforcer extends PermissionEnforcer {
  static final LocalPermissionEnforcer INSTANCE = new LocalPermissionEnforcer();

  private LocalPermissionEnforcer() {}
}
