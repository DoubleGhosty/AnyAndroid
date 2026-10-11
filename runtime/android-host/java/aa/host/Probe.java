package aa.host;

import android.os.Binder;
import android.os.Build;
import android.os.IBinder;
import android.os.ServiceManager;
import android.view.View;
import java.lang.reflect.Method;

/** J0 (ADR-006): host code compiled by javac + D8, running on AOSP's real framework. */
public final class Probe {
  /**
   * Implements one of the two methods of the framework interface (the stub declares only that
   * one): the other answers a neutral value through the VM, logged, instead of
   * AbstractMethodError.
   */
  static final class HalfListener implements View.OnAttachStateChangeListener {
    String last = "aucun";

    @Override
    public void onViewAttachedToWindow(View v) {
      last = "attaché";
    }
  }

  public static void main(String[] args) throws Exception {
    System.out.println("hôte : sdk " + Build.VERSION.SDK_INT + ", " + Build.MANUFACTURER);
    HalfListener listener = new HalfListener();
    View.OnAttachStateChangeListener asInterface = listener;
    asInterface.onViewAttachedToWindow(null);
    Method absent = View.OnAttachStateChangeListener.class.getMethod("onViewDetachedFromWindow", View.class);
    Object result = absent.invoke(listener, (Object) null);
    System.out.println("écouteur : " + listener.last + ", méthode absente -> " + result);
    Runnable lambda = () -> System.out.println("lambda : ok");
    lambda.run();

    // J1: the framework's ServiceManager reaches AnyAndroid's local service manager.
    IBinder demo = new Binder("aa.demo");
    ServiceManager.addService("aa.demo", demo);
    IBinder found = ServiceManager.getService("aa.demo");
    System.out.println("services : aa.demo " + (found == demo ? "-> même objet" : "-> " + found)
        + ", absent -> " + ServiceManager.getService("aa.absent")
        + ", liste " + String.join(",", ServiceManager.listServices()));
  }
}
