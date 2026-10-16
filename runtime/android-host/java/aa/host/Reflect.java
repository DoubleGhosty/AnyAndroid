package aa.host;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;

/**
 * Access to the framework's hidden members (ADR-006): looked up by name and argument count,
 * then by argument types, so that a signature change in the image's framework.jar shows as an
 * explicit error naming the member rather than a silent mismatch.
 */
public final class Reflect {
  private Reflect() {}

  public static Object call(Object target, String name, Object... args) {
    return invoke(target.getClass(), target, name, args);
  }

  public static Object callStatic(Class<?> cls, String name, Object... args) {
    return invoke(cls, null, name, args);
  }

  public static Object newInstance(Class<?> cls, Object... args) {
    for (Constructor<?> c : cls.getDeclaredConstructors()) {
      if (accepts(c.getParameterTypes(), args)) {
        try {
          c.setAccessible(true);
          return c.newInstance(args);
        } catch (InvocationTargetException e) {
          throw rethrow(e.getCause());
        } catch (ReflectiveOperationException e) {
          throw new IllegalStateException("[AnyAndroid] " + cls.getName() + ".<init> : " + e, e);
        }
      }
    }
    throw new IllegalStateException("[AnyAndroid] constructeur introuvable : " + cls.getName() + " (" + args.length + " arguments)");
  }

  public static Object get(Object target, String name) {
    try {
      return field(target.getClass(), name).get(target);
    } catch (IllegalAccessException e) {
      throw new IllegalStateException(e);
    }
  }

  public static void set(Object target, String name, Object value) {
    try {
      field(target.getClass(), name).set(target, value);
    } catch (IllegalAccessException e) {
      throw new IllegalStateException(e);
    }
  }

  public static Object getStatic(Class<?> cls, String name) {
    try {
      return field(cls, name).get(null);
    } catch (IllegalAccessException e) {
      throw new IllegalStateException(e);
    }
  }

  public static void setStatic(Class<?> cls, String name, Object value) {
    try {
      field(cls, name).set(null, value);
    } catch (IllegalAccessException e) {
      throw new IllegalStateException(e);
    }
  }

  public static Class<?> type(String name) {
    try {
      return Class.forName(name);
    } catch (ClassNotFoundException e) {
      throw new IllegalStateException("[AnyAndroid] classe absente du framework : " + name, e);
    }
  }

  private static Field field(Class<?> cls, String name) {
    for (Class<?> c = cls; c != null; c = c.getSuperclass()) {
      try {
        Field f = c.getDeclaredField(name);
        f.setAccessible(true);
        return f;
      } catch (NoSuchFieldException e) {
        // superclass next
      }
    }
    throw new IllegalStateException("[AnyAndroid] champ introuvable : " + cls.getName() + "." + name);
  }

  private static Object invoke(Class<?> cls, Object target, String name, Object[] args) {
    for (Class<?> c = cls; c != null; c = c.getSuperclass()) {
      for (Method m : c.getDeclaredMethods()) {
        if (!m.getName().equals(name) || (target == null) != Modifier.isStatic(m.getModifiers())) continue;
        if (!accepts(m.getParameterTypes(), args)) continue;
        try {
          m.setAccessible(true);
          return m.invoke(target, args);
        } catch (InvocationTargetException e) {
          throw rethrow(e.getCause());
        } catch (IllegalAccessException e) {
          throw new IllegalStateException(e);
        }
      }
    }
    throw new IllegalStateException("[AnyAndroid] méthode introuvable : " + cls.getName() + "." + name + " (" + args.length + " arguments)");
  }

  private static boolean accepts(Class<?>[] params, Object[] args) {
    if (params.length != args.length) return false;
    for (int i = 0; i < params.length; ++i) {
      Object a = args[i];
      Class<?> p = params[i];
      if (a == null) {
        if (p.isPrimitive()) return false;
        continue;
      }
      if (p.isPrimitive()) p = box(p);
      if (!p.isInstance(a)) return false;
    }
    return true;
  }

  private static Class<?> box(Class<?> p) {
    if (p == int.class) return Integer.class;
    if (p == boolean.class) return Boolean.class;
    if (p == long.class) return Long.class;
    if (p == float.class) return Float.class;
    if (p == double.class) return Double.class;
    if (p == char.class) return Character.class;
    if (p == byte.class) return Byte.class;
    return Short.class;
  }

  private static RuntimeException rethrow(Throwable t) {
    if (t instanceof RuntimeException) return (RuntimeException) t;
    if (t instanceof Error) throw (Error) t;
    return new IllegalStateException(t);
  }
}
