package android.app;

import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;

/** Stub of android.app.IRequestFinishCallback for IActivityClientController.onBackPressed. */
public interface IRequestFinishCallback extends IInterface {
  abstract class Stub extends Binder implements IRequestFinishCallback {
    public Stub() {}

    public static IRequestFinishCallback asInterface(IBinder obj) {
      throw new RuntimeException("stub");
    }

    public IBinder asBinder() {
      return this;
    }
  }

  void requestFinish();
}
