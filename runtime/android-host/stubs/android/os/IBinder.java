package android.os;

// Declarations of framework classes for javac only (see runtime/android-host/README.md).
public interface IBinder {
  IInterface queryLocalInterface(String descriptor);
}
