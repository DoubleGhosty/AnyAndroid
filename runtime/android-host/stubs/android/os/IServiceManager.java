package android.os;

public interface IServiceManager extends IInterface {
  Service getService2(String name) throws RemoteException;

  Service checkService2(String name) throws RemoteException;

  void addService(String name, IBinder service, boolean allowIsolated, int dumpPriority) throws RemoteException;

  String[] listServices(int dumpPriority) throws RemoteException;

  boolean isDeclared(String name) throws RemoteException;

  abstract class Stub extends Binder implements IServiceManager {
    public static IServiceManager asInterface(IBinder obj) {
      throw new RuntimeException("stub");
    }

    @Override
    public IBinder asBinder() {
      return this;
    }
  }
}
