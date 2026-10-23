package aa.host;

import java.lang.invoke.CallSite;
import java.lang.invoke.ConstantCallSite;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;

/**
 * Bootstrap methods and targets of IndySites, the class that gen/IndySitesGen.java writes with
 * invokedynamic and ldc of method handles (invoke-custom and const-method-handle after D8).
 */
public final class Indy {
  private Indy() {}

  static int counter = 5;
  static int constantBootstraps;

  static int twice(int x) {
    return 2 * x;
  }

  /** Every kind of static argument, and the Lookup ART creates for the call site. */
  static CallSite describe(MethodHandles.Lookup lookup, String name, MethodType type, int i, long j, float f, double d,
      String s, Class<?> c, MethodHandle mh, MethodType mt) throws Throwable {
    String text = name + " " + type + " ; " + i + " " + j + " " + f + " " + d + " " + s + " " + c.getName() + " "
        + (int) mh.invokeExact(21) + " " + mt + " ; Lookup de " + lookup.lookupClass().getName();
    return new ConstantCallSite(MethodHandles.constant(String.class, text));
  }

  static CallSite constant(MethodHandles.Lookup lookup, String name, MethodType type, MethodHandle target) {
    ++constantBootstraps;
    return new ConstantCallSite(target);
  }

  /** A variable arity bootstrap method: ART collects the trailing static arguments. */
  static CallSite varargs(MethodHandles.Lookup lookup, String name, MethodType type, String... parts) {
    return new ConstantCallSite(MethodHandles.constant(String.class, String.join("+", parts)));
  }

  static CallSite nothing(MethodHandles.Lookup lookup, String name, MethodType type) {
    return null;
  }
}
