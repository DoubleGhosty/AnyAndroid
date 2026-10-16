package android.app;

import android.os.Binder;

public interface IActivityManager {
  abstract class Stub extends Binder implements IActivityManager {
    public Stub(android.os.PermissionEnforcer enforcer) {}
  }

  void attachApplication(IApplicationThread app, long startSeq);

  void reportOptimizationInfo(android.os.IBinder app, String compilerFilter, String compilationReason);

  boolean handleApplicationWtf(
      android.os.IBinder app, String tag, boolean system, ApplicationErrorReport.ParcelableCrashInfo crashInfo,
      int immediateCallerPid);

  int checkPermissionForDevice(String permission, int pid, int uid, int deviceId);

  ContentProviderHolder getContentProvider(
      IApplicationThread caller, String callingPackage, String name, int userId, boolean stable);
}
