package aa.host;

import aa.host.probe.Hidden;
import android.system.Os;
import android.system.OsConstants;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileDescriptor;
import java.io.IOException;
import java.io.ObjectInputStream;
import java.io.ObjectOutputStream;
import java.io.ObjectStreamClass;
import java.io.Serializable;
import java.lang.annotation.Annotation;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.invoke.VarHandle;
import java.lang.invoke.WrongMethodTypeException;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.lang.reflect.TypeVariable;
import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.StandardOpenOption;
import java.time.Instant;
import java.time.ZoneId;
import java.time.ZonedDateTime;
import java.time.format.DateTimeFormatter;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TimeZone;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * Self-test of the libcore paths added to AnyAndroid on 17 October (G2), on AOSP's real libcore:
 * scatter-gather I/O, MethodHandle invocation, annotations and generic signatures, reflection
 * access checks, invoke-custom and method handle constants, serialization, environment and
 * processes, time zones and the remaining POSIX calls. Each line is what Android prints; a
 * failure prints its class.
 */
public final class LibcoreProbe {
  private LibcoreProbe() {}

  interface Check {
    String run() throws Throwable;
  }

  static void check(String what, Check c) {
    String result;
    try {
      result = c.run();
    } catch (Throwable t) {
      result = "échec : " + t.getClass().getName();  // messages differ from ART's
    }
    System.out.println(what + " : " + result);
  }

  public static void main(String[] args) {
    scatterGather();
    methodHandles();
    annotations();
    proxies();
    access();
    invokeDynamic();
    smallNatives();
    bigIntegers();
    threads();
    dexFilesInMemory();
    unicode();
    timeZones();
    posix();
  }

  // --- Scatter-gather I/O (readv, writev) -----------------------------------------------

  static File scratch(String name) {
    return new File(System.getProperty("java.io.tmpdir"), name);
  }

  static String text(ByteBuffer b) {
    b.flip();
    byte[] out = new byte[b.remaining()];
    b.get(out);
    return new String(out, StandardCharsets.UTF_8);
  }

  static void scatterGather() {
    check("FileChannel, écriture groupée", () -> {
      File f = scratch("aa-gather.bin");
      try (FileChannel ch = FileChannel.open(f.toPath(), StandardOpenOption.CREATE, StandardOpenOption.WRITE,
          StandardOpenOption.READ, StandardOpenOption.TRUNCATE_EXISTING)) {
        ByteBuffer direct = ByteBuffer.allocateDirect(5);
        direct.put("jour,".getBytes(StandardCharsets.UTF_8)).flip();
        long written = ch.write(new ByteBuffer[] {
            ByteBuffer.wrap("Bon".getBytes(StandardCharsets.UTF_8)), direct,
            ByteBuffer.wrap(" monde".getBytes(StandardCharsets.UTF_8))});
        ch.position(0);
        ByteBuffer a = ByteBuffer.allocate(4);
        ByteBuffer b = ByteBuffer.allocateDirect(10);
        long read = ch.read(new ByteBuffer[] {a, b});
        return written + " octets écrits, " + read + " lus : « " + text(a) + "|" + text(b) + " »";
      }
    });
    check("Os.writev et Os.readv", () -> {
      File f = scratch("aa-iov.bin");
      FileDescriptor fd = Os.open(f.getPath(), OsConstants.O_RDWR | OsConstants.O_CREAT | OsConstants.O_TRUNC, 0600);
      try {
        byte[] head = "xxAndroid".getBytes(StandardCharsets.UTF_8);
        ByteBuffer tail = ByteBuffer.allocateDirect(8);
        tail.put(" 17".getBytes(StandardCharsets.UTF_8));
        int written = Os.writev(fd, new Object[] {head, tail}, new int[] {2, 0}, new int[] {7, 3});
        Os.lseek(fd, 0, OsConstants.SEEK_SET);
        byte[] first = new byte[4];
        ByteBuffer second = ByteBuffer.allocateDirect(16);
        int read = Os.readv(fd, new Object[] {first, second}, new int[] {0, 2}, new int[] {4, 14});
        byte[] rest = new byte[read - 4];
        for (int i = 0; i < rest.length; i++) rest[i] = second.get(2 + i);
        return written + " écrits, " + read + " lus : « " + new String(first, StandardCharsets.UTF_8) + "|"
            + new String(rest, StandardCharsets.UTF_8) + " »";
      } finally {
        Os.close(fd);
      }
    });
  }

  // --- MethodHandle (invoke-polymorphic) ----------------------------------------------

  static final class Point {
    int x = 4;
    static long created;

    Point() {}

    Point(int x) {
      this.x = x;
      created++;
    }

    int scaled(int by) {
      return x * by;
    }
  }

  static void methodHandles() {
    MethodHandles.Lookup lookup = MethodHandles.lookup();
    check("invokeExact, méthode virtuelle", () -> {
      MethodHandle concat = lookup.findVirtual(String.class, "concat", MethodType.methodType(String.class, String.class));
      return (String) concat.invokeExact("Méthode", "Handle");
    });
    check("invokeExact, type différent", () -> {
      MethodHandle concat = lookup.findVirtual(String.class, "concat", MethodType.methodType(String.class, String.class));
      Object r = (Object) concat.invokeExact("a", "b");
      return "accepté à tort : " + r;
    });
    check("invoke, élargissement int vers long", () -> {
      MethodHandle max = lookup.findStatic(Math.class, "max", MethodType.methodType(long.class, long.class, long.class));
      long r = (long) max.invoke(3, 7L);
      Object boxed = max.invoke(Integer.valueOf(9), 2L);
      return r + " puis " + boxed + " (" + boxed.getClass().getSimpleName() + ")";
    });
    check("invoke, rétrécissement long vers int", () -> {
      MethodHandle max = lookup.findStatic(Math.class, "max", MethodType.methodType(long.class, long.class, long.class));
      int r = (int) max.invoke(1L, 2L);
      return "accepté à tort : " + r;
    });
    check("champs et constructeur", () -> {
      MethodHandle getter = lookup.findGetter(Point.class, "x", int.class);
      MethodHandle setter = lookup.findSetter(Point.class, "x", int.class);
      MethodHandle ctor = lookup.findConstructor(Point.class, MethodType.methodType(void.class, int.class));
      MethodHandle count = lookup.findStaticGetter(Point.class, "created", long.class);
      Point p = (Point) ctor.invokeExact(6);
      setter.invokeExact(p, (int) getter.invokeExact(p) + 1);
      return "x = " + p.x + ", créés " + (long) count.invokeExact();
    });
    check("bindTo et insertArguments", () -> {
      MethodHandle scaled = lookup.findVirtual(Point.class, "scaled", MethodType.methodType(int.class, int.class));
      MethodHandle bound = scaled.bindTo(new Point());
      MethodHandle triple = MethodHandles.insertArguments(scaled, 1, 3);
      return (int) bound.invokeExact(5) + " et " + (int) triple.invokeExact(new Point(2));
    });
    check("asType et filterReturnValue", () -> {
      MethodHandle length = lookup.findVirtual(String.class, "length", MethodType.methodType(int.class));
      MethodHandle generic = length.asType(MethodType.methodType(Object.class, Object.class));
      MethodHandle toStr = lookup.findStatic(Integer.class, "toString", MethodType.methodType(String.class, int.class));
      MethodHandle described = MethodHandles.filterReturnValue(length, toStr);
      return generic.invoke((Object) "quatre") + " et « " + (String) described.invokeExact("cinq!") + " »";
    });
  }

  // --- Annotations and generic signatures ------------------------------------------------

  @Retention(RetentionPolicy.RUNTIME)
  @interface Note {
    String value();

    int level() default 3;

    String[] tags() default {};
  }

  @Retention(RetentionPolicy.RUNTIME)
  @interface Marker {}

  @Note(value = "classe", tags = {"a", "b"})
  @Marker
  static class Annotated {
    @Note("champ")
    List<String> names;

    @Note(value = "méthode", level = 7)
    <T extends Number> Map<String, T> pick(@Note("paramètre") T x, int y) {
      return null;
    }
  }

  static void annotations() {
    check("annotation de classe", () -> {
      Note n = Annotated.class.getAnnotation(Note.class);
      return n.value() + ", niveau " + n.level() + ", étiquettes " + Arrays.toString(n.tags()) + ", Marker "
          + Annotated.class.isAnnotationPresent(Marker.class) + ", toutes "
          + Annotated.class.getDeclaredAnnotations().length;
    });
    check("toString, equals, hashCode", () -> {
      Note a = Annotated.class.getAnnotation(Note.class);
      Note b = Annotated.class.getAnnotation(Note.class);
      return a + ", égales " + a.equals(b) + ", même hash " + (a.hashCode() == b.hashCode()) + ", type "
          + a.annotationType().getSimpleName();
    });
    check("champ et méthode", () -> {
      Field f = Annotated.class.getDeclaredField("names");
      Method m = Annotated.class.getDeclaredMethod("pick", Number.class, int.class);
      Annotation[][] params = m.getParameterAnnotations();
      return f.getAnnotation(Note.class).value() + ", " + m.getAnnotation(Note.class).value() + " niveau "
          + m.getAnnotation(Note.class).level() + ", paramètres " + ((Note) params[0][0]).value() + " / "
          + params[1].length;
    });
    check("signatures génériques", () -> {
      Field f = Annotated.class.getDeclaredField("names");
      Method m = Annotated.class.getDeclaredMethod("pick", Number.class, int.class);
      TypeVariable<Method> t = m.getTypeParameters()[0];
      return f.getGenericType() + " ; " + m.getGenericReturnType() + " ; " + t.getName() + " étend "
          + Arrays.toString(t.getBounds()) + " ; paramètre " + m.getGenericParameterTypes()[0];
    });
  }

  // --- Dynamic proxies (java.lang.reflect.Proxy, which ART's annotations rely on) ---------

  static void proxies() {
    check("Proxy.newProxyInstance", () -> {
      InvocationHandler h = (proxy, method, args) ->
          method.getName().equals("compareTo") ? Integer.valueOf(((String) args[0]).length()) : null;
      @SuppressWarnings("unchecked")
      Comparable<String> c = (Comparable<String>) Proxy.newProxyInstance(
          LibcoreProbe.class.getClassLoader(), new Class<?>[] {Comparable.class}, h);
      return "compareTo(« seize ») = " + c.compareTo("seize") + ", classe mandataire "
          + Proxy.isProxyClass(c.getClass());
    });
  }

  // --- Reflection access checks (ART's VerifyAccess, for members not opened by
  // setAccessible) ---------------------------------------------------------------------------

  /** The message of the IllegalAccessException that `c` throws, which is ART's. */
  static String denied(Check c) throws Throwable {
    try {
      return "permis : " + c.run();
    } catch (IllegalAccessException e) {
      return e.getMessage();
    }
  }

  private static String own() {
    return "privée";
  }

  /** A subclass of Hidden in another package: protected members, as ART checks them. */
  static final class Sub extends Hidden {
    static String probe() throws Throwable {
      Field depth = Hidden.class.getDeclaredField("depth");
      Field level = Hidden.class.getDeclaredField("level");
      return "depth " + denied(() -> String.valueOf(depth.getInt(new Sub()))) + " ; level "
          + denied(() -> String.valueOf(level.getInt(null)));
    }
  }

  static void access() {
    check("méthode privée d'une autre classe", () -> {
      Method m = Hidden.class.getDeclaredMethod("secret");
      String before = denied(() -> (String) m.invoke(null));
      m.setAccessible(true);
      return before + " ; après setAccessible : " + m.invoke(null);
    });
    check("méthode privée de la même classe", () ->
        denied(() -> (String) LibcoreProbe.class.getDeclaredMethod("own").invoke(null)));
    check("champ de paquet d'une autre classe", () ->
        denied(() -> String.valueOf(Hidden.class.getDeclaredField("packageCount").getInt(new Sub()))));
    check("champs protégés depuis une sous-classe", Sub::probe);
    check("classe de paquet d'un autre paquet", () -> denied(() ->
        String.valueOf(Class.forName("aa.host.probe.PackagePrivate").getDeclaredConstructor().newInstance())));
    check("constructeur protégé d'une classe publique", () -> denied(() ->
        Hidden.class.getDeclaredConstructor().newInstance().getClass().getName()));
    // Class methods whose libcore bytecode reads ART fields (accessFlags, ifTable...).
    check("AtomicIntegerFieldUpdater", () -> {
      java.util.concurrent.atomic.AtomicIntegerFieldUpdater<Counter> updater =
          java.util.concurrent.atomic.AtomicIntegerFieldUpdater.newUpdater(Counter.class, "value");
      Counter c = new Counter();
      updater.incrementAndGet(c);
      String other;
      try {
        java.util.concurrent.atomic.AtomicIntegerFieldUpdater.newUpdater(Hidden.class, "packageCount");
        other = "permis";
      } catch (RuntimeException e) {  // the IllegalAccessException, wrapped
        other = String.valueOf(e.getCause() == null ? e : e.getCause().getClass().getName());
      }
      return updater.addAndGet(c, 41) + ", champ de paquet d'un autre paquet : " + other;
    });
    check("findVirtual d'une méthode par défaut", () -> {
      MethodHandle stream = MethodHandles.lookup().findVirtual(java.util.AbstractCollection.class, "stream",
          MethodType.methodType(java.util.stream.Stream.class));
      return String.valueOf(((java.util.stream.Stream<?>) stream.invoke(new java.util.ArrayList<>(List.of(1, 2, 3)))).count());
    });
    check("getClasses, componentType, descriptorString", () -> {
      List<String> names = new java.util.ArrayList<>();
      for (Class<?> c : java.util.AbstractMap.class.getClasses()) names.add(c.getSimpleName());
      java.util.Collections.sort(names);
      return names + ", " + int[][].class.componentType().getName() + ", " + String[].class.descriptorString() + " "
          + int.class.descriptorString() + ", drapeaux " + Integer.toHexString(Runnable.class.getModifiers());
    });
  }

  static final class Counter {
    volatile int value;
  }

  // --- invoke-custom, const-method-handle and const-method-type (IndySites, written by
  // gen/IndySitesGen.java, then D8) ----------------------------------------------------------

  static void invokeDynamic() {
    check("invoke-custom, arguments statiques", IndySites::describe);
    check("invoke-custom, cible appelée", () -> IndySites.twice(4) + " puis " + IndySites.twice(5)
        + ", amorce appelée " + Indy.constantBootstraps + " fois");
    check("invoke-custom, amorce à arité variable", IndySites::joined);
    check("invoke-custom, amorce qui rend null", () -> {
      try {
        return "rendu " + IndySites.broken();
      } catch (BootstrapMethodError e) {
        return e.getMessage() + " ; cause " + e.getCause();
      }
    });
    check("const-method-handle, méthode statique", () -> {
      MethodHandle h = IndySites.handle();
      return h.type() + " : " + (int) h.invokeExact(8) + ", nouvel objet " + (h != IndySites.handle());
    });
    check("const-method-type", () -> {
      MethodType t = IndySites.type();
      return t + ", interné " + (t == MethodType.methodType(long.class, int.class, String.class));
    });
    check("const-method-handle, champ statique", () -> {
      MethodHandle h = IndySites.getter();
      return h.type() + " : " + (int) h.invokeExact();
    });
    check("const-method-handle, méthodes d'instance et d'interface", () -> {
      MethodHandle v = IndySites.virtualHandle();
      MethodHandle i = IndySites.interfaceHandle();
      return v.type() + " : " + (int) v.invokeExact("seize") + " ; " + i.type() + " : "
          + (int) i.invokeExact((CharSequence) "abc");
    });
    check("const-method-handle, constructeur", () -> {
      MethodHandle c = IndySites.constructorHandle();
      return c.type() + " : " + (StringBuilder) c.invokeExact("abc");
    });
  }

  // --- Smaller natives: serialization, environment, processes, reflective refusals -------

  /** Serializable without serialVersionUID: the default one covers its static initializer. */
  static final class SerialPoint implements Serializable {
    static final List<String> NAMES = new java.util.ArrayList<>();
    float x = Float.NaN;
    double[] ys = {1.5, Double.NEGATIVE_INFINITY};
  }

  /** The state of `t` once it reaches `want` (at most two seconds later). */
  static String stateOf(Thread t, Thread.State want) throws InterruptedException {
    for (int i = 0; i < 2000 && t.getState() != want; i++) Thread.sleep(1);
    return t.getState().toString();
  }

  static void smallNatives() {
    check("sérialisation de float[] et double[]", () -> {
      ByteArrayOutputStream bytes = new ByteArrayOutputStream();
      try (ObjectOutputStream out = new ObjectOutputStream(bytes)) {
        out.writeObject(new float[] {1.25f, Float.intBitsToFloat(0x7fc00001)});
        out.writeObject(new double[] {-0.0, Math.PI});
      }
      try (ObjectInputStream in = new ObjectInputStream(new ByteArrayInputStream(bytes.toByteArray()))) {
        float[] f = (float[]) in.readObject();
        double[] d = (double[]) in.readObject();
        return f[0] + ", NaN 0x" + Integer.toHexString(Float.floatToRawIntBits(f[1])) + ", " + d[0] + ", " + d[1]
            + " ; " + bytes.size() + " octets";
      }
    });
    check("Throwable sérialisé", () -> {
      IllegalStateException original = new IllegalStateException("état", new IOException("disque"));
      original.addSuppressed(new RuntimeException("supprimée"));
      ByteArrayOutputStream bytes = new ByteArrayOutputStream();
      try (ObjectOutputStream out = new ObjectOutputStream(bytes)) {
        out.writeObject(original);
        out.writeObject(new ClassNotFoundException("absente", new IOException("lecture")));
      }
      try (ObjectInputStream in = new ObjectInputStream(new ByteArrayInputStream(bytes.toByteArray()))) {
        Throwable back = (Throwable) in.readObject();
        Throwable missing = (Throwable) in.readObject();
        return back + ", cause " + back.getCause() + ", supprimées " + Arrays.toString(back.getSuppressed())
            + ", pile intacte " + Arrays.equals(original.getStackTrace(), back.getStackTrace()) + " ; " + missing.getCause();
      }
    });
    check("initCause et setStackTrace", () -> {
      Exception e = new Exception("a");
      e.initCause(null);
      String overwrite;
      try {
        e.initCause(new Error("b"));
        overwrite = "permis";
      } catch (IllegalStateException ise) {
        overwrite = ise.getMessage() + " (cause " + (ise.getCause() == e) + ")";
      }
      Exception self = new Exception();
      String selfCause;
      try {
        self.initCause(self);
        selfCause = "permis";
      } catch (IllegalArgumentException iae) {
        selfCause = iae.getMessage();
      }
      Exception fixed = new Exception("fixe");
      fixed.setStackTrace(new StackTraceElement[] {new StackTraceElement("a.B", "c", "B.java", 7)});
      Exception frozen = new Exception("figée", null, true, false) {};
      frozen.setStackTrace(fixed.getStackTrace());
      return overwrite + " ; " + selfCause + " ; " + Arrays.toString(fixed.getStackTrace()) + " ; figée "
          + frozen.getStackTrace().length + ", pile de création " + e.getStackTrace()[0].getClassName();
    });
    check("printStackTrace(PrintWriter)", () -> {
      java.io.StringWriter text = new java.io.StringWriter();
      Exception e = new Exception("haut", new Error("bas"));
      e.addSuppressed(new RuntimeException("à côté"));
      e.printStackTrace(new java.io.PrintWriter(text, true));
      List<String> kept = new java.util.ArrayList<>();
      for (String line : text.toString().split("\n")) {
        if (!line.startsWith("\tat ") && !line.startsWith("\t\tat ") && !line.trim().startsWith("...")) kept.add(line.trim());
      }
      return String.join(" | ", kept);
    });
    check("serialVersionUID par défaut", () -> String.valueOf(ObjectStreamClass.lookup(SerialPoint.class).getSerialVersionUID()));
    check("System.getenv", () -> System.getenv("ANDROID_ROOT") + ", " + System.getenv().get("ANDROID_DATA")
        + ", PATH présent " + System.getenv().containsKey("PATH"));
    check("ProcessBuilder.start", () -> {
      try {
        new ProcessBuilder("sh", "-c", "true").start();
        return "processus lancé";
      } catch (IOException e) {
        return e.getMessage();
      }
    });
    check("annotation d'un paramètre", () -> {
      Method m = Annotated.class.getDeclaredMethod("pick", Number.class, int.class);
      return m.getParameters()[0].getAnnotation(Note.class).value() + ", second " + m.getParameters()[1].getAnnotation(Note.class);
    });
    check("Thread.getState", () -> {
      Object lock = new Object();
      Thread waiter = new Thread(() -> {
        synchronized (lock) {
          try {
            lock.wait();
          } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
          }
        }
      });
      String created = waiter.getState().toString();
      waiter.start();
      String waiting = stateOf(waiter, Thread.State.WAITING);
      Thread sleeper = new Thread(() -> {
        try {
          Thread.sleep(60_000);
        } catch (InterruptedException e) {
          Thread.currentThread().interrupt();
        }
      });
      sleeper.start();
      String timed = stateOf(sleeper, Thread.State.TIMED_WAITING);
      Thread blocked = new Thread(() -> {
        synchronized (lock) {
          lock.notifyAll();
        }
      });
      String entering;
      synchronized (lock) {
        blocked.start();
        entering = stateOf(blocked, Thread.State.BLOCKED);
      }
      sleeper.interrupt();
      blocked.join();
      waiter.join();
      sleeper.join();
      return created + " " + waiting + " " + timed + " " + entering + " " + waiter.getState();
    });
    check("ThreadGroup", () -> {
      ThreadGroup g = Thread.currentThread().getThreadGroup();
      return g.getName() + " (parent " + g.getParent().getName() + "), nouveau fil dans "
          + new Thread(() -> {}).getThreadGroup().getName();
    });
    check("VarHandle.get par réflexion", () -> {
      VarHandle vh = MethodHandles.arrayElementVarHandle(int[].class);
      try {
        VarHandle.class.getMethod("get", Object[].class).invoke(vh, (Object) new Object[] {new int[1], 0});
        return "appelé";
      } catch (InvocationTargetException e) {
        return String.valueOf(e.getCause());
      }
    });
  }

  // --- BigInteger's native paths (NativeBN: multiply from 50 ints, division from a 40-int
  // divisor, modPow for a modulus of 3 ints or more) -----------------------------------------

  static void bigIntegers() {
    BigInteger a = BigInteger.valueOf(3).pow(1500);
    BigInteger b = BigInteger.valueOf(7).pow(900).add(BigInteger.ONE);
    BigInteger e18 = BigInteger.TEN.pow(18);
    check("BigInteger.multiply", () -> {
      BigInteger p = a.multiply(b);
      return p.bitLength() + " bits, modulo 10^18 : " + p.mod(e18);
    });
    check("BigInteger.divideAndRemainder", () -> {
      BigInteger[] qr = a.multiply(b).add(BigInteger.valueOf(12345)).negate().divideAndRemainder(b);
      return "quotient -3^1500 " + qr[0].equals(a.negate()) + ", reste " + qr[1];
    });
    check("BigInteger.modPow", () -> {
      BigInteger m = BigInteger.ONE.shiftLeft(2203).subtract(BigInteger.ONE);  // Mersenne prime
      return "Fermat " + BigInteger.valueOf(3).modPow(m.subtract(BigInteger.ONE), m) + ", 5^(10^30) modulo 10^18 : "
          + BigInteger.valueOf(5).modPow(BigInteger.TEN.pow(30), m).mod(e18) + ", -2^-1 : "
          + BigInteger.valueOf(-2).modPow(BigInteger.ONE.negate(), BigInteger.TEN.pow(21).add(BigInteger.ONE));
    });
  }

  // --- Threads (sleep, stacks, constructors) ---------------------------------------------

  static String frame(StackTraceElement e) {
    return e.getClassName() + "." + e.getMethodName() + (e.isNativeMethod() ? " (natif)" : "");
  }

  static void threads() {
    // Java 19 API, reached by reflection (the host sources compile for Java 17).
    check("Thread.sleep(Duration)", () -> {
      long start = System.nanoTime();
      Thread.class.getMethod("sleep", java.time.Duration.class).invoke(null, java.time.Duration.ofMillis(5));
      return "au moins 5 ms : " + (System.nanoTime() - start >= 5_000_000L);
    });
    check("pile du fil courant", () -> {
      StackTraceElement[] stack = Thread.currentThread().getStackTrace();
      boolean main = false;
      for (StackTraceElement e : stack) main |= e.getClassName().equals(LibcoreProbe.class.getName()) && e.getMethodName().equals("main");
      return frame(stack[0]) + ", " + frame(stack[1]) + ", main présent " + main;
    });
    check("pile d'un autre fil", () -> {
      Object lock = new Object();
      Thread waiter = new Thread(() -> {
        synchronized (lock) {
          try {
            lock.wait();
          } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
          }
        }
      }, "attente");
      waiter.start();
      stateOf(waiter, Thread.State.WAITING);
      StackTraceElement[] stack = waiter.getStackTrace();
      Map<Thread, StackTraceElement[]> all = Thread.getAllStackTraces();
      String result = frame(stack[0]) + ", toutes les piles : fil courant " + all.containsKey(Thread.currentThread())
          + ", attente " + (all.get(waiter) != null && all.get(waiter).length == stack.length);
      waiter.interrupt();
      waiter.join();
      return result + ", après la fin " + waiter.getStackTrace().length;
    });
    check("Thread(groupe, cible, nom, pile, héritage)", () -> {
      InheritableThreadLocal<String> local = new InheritableThreadLocal<>();
      local.set("hérité");
      String[] seen = new String[2];
      Thread t = new Thread(null, () -> seen[0] = Thread.currentThread().getName() + " " + local.get(), "fil-5", 0, false);
      t.setPriority(Thread.MAX_PRIORITY);
      t.start();
      t.join();
      Thread child = new Thread(() -> seen[1] = local.get());
      child.start();
      child.join();
      return seen[0] + ", priorité " + t.getPriority() + ", " + t.getState() + ", groupe nul " + (t.getThreadGroup() == null)
          + ", fil ordinaire " + seen[1];
    });
    check("Thread.toString et identifiant", () -> {
      Thread t = new Thread(() -> {}, "nommé");
      long id = t.getId();
      String before = t.toString();
      t.start();
      t.join();
      return before + " puis " + t + ", identifiant stable " + (id == t.getId()) + ", main "
          + Thread.currentThread().getName() + " " + (Thread.currentThread().getId() > 0);
    });
    // Java 21 APIs, reached by reflection: libcore's own constructors and ThreadContainer.
    check("Thread.ofPlatform", () -> {
      Class<?> ofPlatform = Class.forName("java.lang.Thread$Builder$OfPlatform");
      Object builder = Thread.class.getMethod("ofPlatform").invoke(null);
      builder = ofPlatform.getMethod("name", String.class, long.class).invoke(builder, "plate-", 7L);
      builder = ofPlatform.getMethod("daemon", boolean.class).invoke(builder, true);
      builder = ofPlatform.getMethod("priority", int.class).invoke(builder, 3);
      String[] seen = new String[1];
      Thread t = (Thread) ofPlatform.getMethod("unstarted", Runnable.class)
          .invoke(builder, (Runnable) () -> seen[0] = Thread.currentThread().getName());
      String made = t.getName() + " démon " + t.isDaemon() + " priorité " + t.getPriority() + " " + t.getState();
      t.start();
      t.join();
      return made + ", exécuté par " + seen[0] + ", " + t.getState();
    });
    check("Executors.newThreadPerTaskExecutor", () -> {
      java.util.concurrent.ThreadFactory factory = r -> new Thread(r, "par-tâche");
      java.util.concurrent.ExecutorService executor = (java.util.concurrent.ExecutorService) java.util.concurrent.Executors.class
          .getMethod("newThreadPerTaskExecutor", java.util.concurrent.ThreadFactory.class).invoke(null, factory);
      String name = executor.submit(() -> Thread.currentThread().getName()).get();
      executor.shutdown();
      return name + ", terminé " + executor.awaitTermination(10, java.util.concurrent.TimeUnit.SECONDS);
    });
  }

  // --- DEX files in memory (InMemoryDexClassLoader) ----------------------------------------

  // Two command jars of the image, outside the boot class path: one per loader (each
  // InMemoryDexClassLoader defines its own classes), from the heap then from native memory.
  static void dexFilesInMemory() {
    inMemory("abx.jar", "com.android.commands.abx.Abx", false);
    inMemory("appwidget.jar", "com.android.commands.appwidget.AppWidget", true);
  }

  static void inMemory(String jar, String className, boolean direct) {
    check("InMemoryDexClassLoader, tampon " + (direct ? "direct" : "du tas"), () -> {
      byte[] dex;
      try (ZipFile zip = new ZipFile("/system/framework/" + jar)) {
        ZipEntry entry = zip.getEntry("classes.dex");
        dex = zip.getInputStream(entry).readAllBytes();
      }
      ByteBuffer buffer = direct ? ByteBuffer.allocateDirect(dex.length + 8) : ByteBuffer.allocate(dex.length + 8);
      buffer.position(8);  // the DEX starts at the buffer's position, not at 0
      buffer.put(dex).flip().position(8);
      ClassLoader loader = (ClassLoader) Class.forName("dalvik.system.InMemoryDexClassLoader")
          .getConstructor(ByteBuffer.class, ClassLoader.class).newInstance(buffer, String.class.getClassLoader());
      Class<?> c = loader.loadClass(className);
      return c.getSimpleName() + " par " + c.getClassLoader().getClass().getSimpleName() + ", même chargeur "
          + (c.getClassLoader() == loader) + ", main " + Arrays.toString(c.getMethod("main", String[].class).getParameterTypes());
    });
  }

  // --- Character and locales outside ASCII (ICU data of the i18n module) -------------------

  static void unicode() {
    check("Character hors ASCII", () -> Character.isLetter('é') + " " + Character.isDigit('\u0663') + " "
        + Character.getNumericValue('\u216B') + " " + Character.getNumericValue('\u00BD') + " " + Character.isMirrored('\u00AB')
        + " " + Character.isIdeographic('\u4E2D') + " " + Character.isTitleCase('\u01C5') + " "
        + (int) Character.toTitleCase('\u01C6') + " " + Character.isUnicodeIdentifierStart('\u2115') + " "
        + Character.isSpaceChar('\u3000') + " " + Character.getType('\u20AC'));
    check("noms de caractères", () -> Character.getName(0) + " ; " + Character.getName('é') + " ; "
        + Character.codePointOf("GREEK SMALL LETTER ALPHA"));
    check("casse selon la langue", () -> "i".toUpperCase(new Locale("tr")) + " " + "\u0130".toLowerCase(new Locale("tr")).length()
        + " " + "ΟΔΟΣ".toLowerCase(Locale.ROOT) + " " + "straße".toUpperCase(Locale.ROOT));
    check("noms de locales", () -> Locale.FRANCE.getDisplayCountry(Locale.GERMAN) + " ; "
        + Locale.FRANCE.getDisplayLanguage(Locale.JAPANESE) + " ; " + Locale.forLanguageTag("zh-Hant-TW").getDisplayScript(Locale.ENGLISH)
        + " ; " + Locale.FRANCE.getISO3Country() + " " + Locale.FRANCE.getISO3Language());
    check("listes de locales", () -> "pays " + Locale.getISOCountries().length + ", langues " + Locale.getISOLanguages().length
        + ", locales " + (Locale.getAvailableLocales().length > 100));
    check("versions ICU (propriétés / ICU4J)", () -> {
      Object icu = Class.forName("android.icu.util.VersionInfo").getField("ICU_VERSION").get(null);
      Object unicode = Class.forName("android.icu.lang.UCharacter").getMethod("getUnicodeVersion").invoke(null);
      Object cldr = Class.forName("android.icu.util.LocaleData").getMethod("getCLDRVersion").invoke(null);
      return System.getProperty("android.icu.library.version") + " / " + icu + ", Unicode "
          + System.getProperty("android.icu.unicode.version") + " / " + unicode + ", CLDR "
          + System.getProperty("android.icu.cldr.version") + " / " + cldr;
    });
    check("noms de fuseaux", () -> {
      for (String[] row : java.text.DateFormatSymbols.getInstance(Locale.FRANCE).getZoneStrings()) {
        if (row[0].equals("Europe/Paris")) return Arrays.toString(row);
      }
      return "Europe/Paris absent";
    });
  }

  // --- Time zones (tzdata of the image, read by the i18n module's Memory) -----------------

  static void timeZones() {
    check("TimeZone Europe/Paris", () -> {
      TimeZone paris = TimeZone.getTimeZone("Europe/Paris");
      long winter = Instant.parse("2026-01-15T12:00:00Z").toEpochMilli();
      long summer = Instant.parse("2026-07-15T12:00:00Z").toEpochMilli();
      return "hiver " + paris.getOffset(winter) / 3600000 + " h, été " + paris.getOffset(summer) / 3600000
          + " h, heure d'été " + paris.useDaylightTime();
    });
    check("java.time America/New_York", () -> {
      ZonedDateTime t = Instant.parse("2026-03-08T06:30:00Z").atZone(ZoneId.of("America/New_York"));
      ZonedDateTime later = t.plusHours(1);
      DateTimeFormatter f = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm xxx");
      return t.format(f) + " puis " + later.format(f);
    });
    check("zone inconnue", () -> TimeZone.getTimeZone("Mars/Olympus").getID());
  }

  // --- Remaining POSIX calls ---------------------------------------------------------------

  static void posix() {
    check("dup, dup2, fcntl, ioctl", () -> {
      File f = scratch("aa-posix.bin");
      FileDescriptor fd = Os.open(f.getPath(), OsConstants.O_RDWR | OsConstants.O_CREAT | OsConstants.O_TRUNC, 0600);
      try {
        Os.writev(fd, new Object[] {"0123456789".getBytes(StandardCharsets.UTF_8)}, new int[] {0}, new int[] {10});
        Os.lseek(fd, 3, OsConstants.SEEK_SET);
        FileDescriptor copy = Os.dup(fd);
        int available = Os.ioctlInt(copy, OsConstants.FIONREAD);
        int cloexec = Os.fcntlInt(copy, OsConstants.F_GETFD, 0) & OsConstants.FD_CLOEXEC;
        Os.close(copy);
        return "FIONREAD " + available + ", FD_CLOEXEC de la copie " + (cloexec != 0);
      } finally {
        Os.close(fd);
      }
    });
    check("getpgid, umask", () -> {
      boolean group = Os.getpgid(0) > 0;
      int old = Os.umask(022);
      int back = Os.umask(old);
      return "groupe " + group + ", umask rendu " + Integer.toOctalString(back);
    });
    // Android 17: OsConstantsHolder.initConstants fills these from bionic's headers.
    check("OsConstantsHolder et sysconf", () -> "AI_ADDRCONFIG 0x" + Integer.toHexString(OsConstants.AI_ADDRCONFIG)
        + ", EAI_NONAME " + OsConstants.EAI_NONAME + ", SIGRTMIN " + OsConstants.SIGRTMIN
        + ", O_NOFOLLOW 0" + Integer.toOctalString(OsConstants.O_NOFOLLOW)
        + ", sysconf(_SC_PAGESIZE = 0x" + Integer.toHexString(OsConstants._SC_PAGESIZE) + ") " + Os.sysconf(OsConstants._SC_PAGESIZE));
  }
}
