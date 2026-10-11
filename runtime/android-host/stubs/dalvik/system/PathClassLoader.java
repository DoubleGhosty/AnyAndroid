package dalvik.system;

public class PathClassLoader extends BaseDexClassLoader {
  public PathClassLoader(String dexPath, ClassLoader parent) {
    super(null, null, null, null);
  }
}
