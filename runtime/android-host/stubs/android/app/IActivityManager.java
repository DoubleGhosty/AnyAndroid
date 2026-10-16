package android.app;

import android.os.Binder;

public interface IActivityManager {
  abstract class Stub extends Binder implements IActivityManager {
    public Stub(android.os.PermissionEnforcer enforcer) {}
  }

  ContentProviderHolder getContentProvider(
      IApplicationThread caller, String callingPackage, String name, int userId, boolean stable);
}
