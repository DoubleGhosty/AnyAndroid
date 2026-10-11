package aa.host;

import android.os.Binder;
import android.os.Build;
import android.os.IBinder;
import android.os.ServiceManager;
import android.view.View;
import dalvik.system.DexFile;
import dalvik.system.PathClassLoader;
import java.lang.reflect.Method;
import java.util.Collections;

/** Self-test of the ADR-006 milestones: host code compiled by javac + D8, on AOSP's real framework. */
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

    // J2: a class of the APK, defined by the PathClassLoader that loaded it.
    String apk = System.getProperty("java.class.path");
    ClassLoader boot = Probe.class.getClassLoader();
    PathClassLoader loader = new PathClassLoader(apk, boot);
    String name = "com.anyandroid.samples.hello.Hello";
    Class<?> hello = loader.loadClass(name);
    Class<?> again = Class.forName(name, false, loader);
    String fromBoot;
    try {
      Class.forName(name, false, null);
      fromBoot = "la voit";
    } catch (ClassNotFoundException e) {
      fromBoot = "ne la voit pas";
    }
    DexFile dex = new DexFile(apk);
    String entries = String.join(",", Collections.list(dex.entries()));
    dex.close();
    System.out.println("chargeurs : " + apk);
    System.out.println("  Hello -> " + (hello.getClassLoader() == loader ? "son PathClassLoader" : hello.getClassLoader())
        + ", forName " + (again == hello ? "même classe" : "autre classe")
        + ", amorçage " + fromBoot + ", Probe -> " + boot.getClass().getSimpleName()
        + ", entrées " + entries);
  }
}
