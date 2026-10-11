package android.permission;

import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.PermissionEnforcer;
import android.os.RemoteException;
import java.util.List;

// Only the methods AnyAndroid implements (the others answer neutral values).
public interface IPermissionManager extends IInterface {
  List getSplitPermissions(boolean upgrade) throws RemoteException;

  int checkPermission(String packageName, String permissionName, String persistentDeviceId, int userId)
      throws RemoteException;

  int checkUidPermission(int uid, String permissionName, int deviceId) throws RemoteException;

  abstract class Stub extends Binder implements IPermissionManager {
    public Stub(PermissionEnforcer enforcer) {}

    @Override
    public IBinder asBinder() {
      return this;
    }
  }
}
