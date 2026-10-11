package android.content.pm;

import android.content.ComponentName;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.PermissionEnforcer;
import android.os.RemoteException;

// Only the methods AnyAndroid implements (the others answer neutral values).
public interface IPackageManager extends IInterface {
  ApplicationInfo getApplicationInfo(String packageName, long flags, int userId) throws RemoteException;

  PackageInfo getPackageInfo(String packageName, long flags, int userId) throws RemoteException;

  ActivityInfo getActivityInfo(ComponentName className, long flags, int userId) throws RemoteException;

  ActivityInfo getReceiverInfo(ComponentName className, long flags, int userId) throws RemoteException;

  ServiceInfo getServiceInfo(ComponentName className, long flags, int userId) throws RemoteException;

  ProviderInfo getProviderInfo(ComponentName className, long flags, int userId) throws RemoteException;

  ResolveInfo resolveIntent(Intent intent, String resolvedType, long flags, int userId) throws RemoteException;

  ParceledListSlice queryIntentActivities(Intent intent, String resolvedType, long flags, int userId)
      throws RemoteException;

  PackageInfoList getInstalledPackages(long flags, int userId) throws RemoteException;

  int getPackageUid(String packageName, long flags, int userId) throws RemoteException;

  String[] getPackagesForUid(int uid) throws RemoteException;

  String getNameForUid(int uid) throws RemoteException;

  int checkPermission(String permName, String pkgName, int userId) throws RemoteException;

  int checkUidPermission(String permName, int uid) throws RemoteException;

  boolean hasSystemFeature(String name, int version) throws RemoteException;

  ParceledListSlice getSystemAvailableFeatures() throws RemoteException;

  int getTargetSdkVersion(String packageName) throws RemoteException;

  boolean isPackageAvailable(String packageName, int userId) throws RemoteException;

  int getComponentEnabledSetting(ComponentName componentName, int userId) throws RemoteException;

  int getApplicationEnabledSetting(String packageName, int userId) throws RemoteException;

  boolean isSafeMode() throws RemoteException;

  boolean isInstantApp(String packageName, int userId) throws RemoteException;

  abstract class Stub extends Binder implements IPackageManager {
    public Stub(PermissionEnforcer enforcer) {}

    @Override
    public IBinder asBinder() {
      return this;
    }
  }
}
