package android.app;

import android.content.Intent;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;

public interface IActivityTaskManager {
  abstract class Stub extends Binder implements IActivityTaskManager {
    public Stub(android.os.PermissionEnforcer enforcer) {}

    public static IActivityTaskManager asInterface(android.os.IBinder obj) {
      throw new RuntimeException("stub");
    }
  }

  IActivityClientController getActivityClientController();

  int startActivity(
      IApplicationThread caller,
      String callingPackage,
      String callingFeatureId,
      Intent intent,
      String resolvedType,
      IBinder resultTo,
      String resultWho,
      int requestCode,
      int flags,
      ProfilerInfo profilerInfo,
      Bundle options);
}
