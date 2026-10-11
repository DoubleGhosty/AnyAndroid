package android.os;

public class Binder implements IBinder {
  public Binder() {}

  public Binder(String descriptor) {}

  public void attachInterface(IInterface owner, String descriptor) {}

  @Override
  public IInterface queryLocalInterface(String descriptor) {
    return null;
  }
}
