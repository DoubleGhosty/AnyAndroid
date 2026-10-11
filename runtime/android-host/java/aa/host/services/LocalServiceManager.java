package aa.host.services;

import android.os.IBinder;
import android.os.IServiceManager;
import android.os.Service;
import android.os.ServiceWithMetadata;
import java.util.Map;
import java.util.TreeMap;

/**
 * The process's service manager (ADR-006): the role servicemanager plays on a phone. The
 * framework reaches it through BinderInternal.getContextObject() and
 * ServiceManagerProxy.getNativeServiceManager(), both answered by AnyAndroid with this object;
 * IServiceManager.Stub.asInterface then finds it as a local interface, so calls are direct.
 */
public final class LocalServiceManager extends IServiceManager.Stub {
  private final Map<String, IBinder> services = new TreeMap<>();

  public LocalServiceManager() {}

  @Override
  public synchronized Service getService2(String name) {
    return checkService2(name);
  }

  /** Like servicemanager: a missing service is a Service holding null, not null itself. */
  @Override
  public synchronized Service checkService2(String name) {
    ServiceWithMetadata found = new ServiceWithMetadata();
    found.service = name == null ? null : services.get(name);
    found.isLazyService = false;
    return Service.serviceWithMetadata(found);
  }

  @Override
  public synchronized void addService(String name, IBinder service, boolean allowIsolated, int dumpPriority) {
    if (name == null || service == null) throw new IllegalArgumentException("service ou nom null");
    services.put(name, service);
  }

  @Override
  public synchronized String[] listServices(int dumpPriority) {
    return services.keySet().toArray(new String[0]);
  }

  @Override
  public synchronized boolean isDeclared(String name) {
    return name != null && services.containsKey(name);
  }
}
