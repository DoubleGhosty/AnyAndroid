package aa.host.services;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import java.util.HashMap;
import java.util.Map;

/**
 * The "settings" content provider (ADR-006 J4): Settings.Global, Secure, System and the
 * DeviceConfig flags (Config), in memory, through the call() protocol of the platform's
 * SettingsProvider (GET_, PUT_, DELETE_ and LIST_config). Values not set read as null, so the
 * framework's defaults apply. Nothing persists yet; there is no generation tracking (callers
 * then read again each time).
 */
public final class SettingsProvider extends ContentProvider {
  /** Settings.AUTHORITY. */
  public static final String AUTHORITY = "settings";

  private final Map<String, Map<String, String>> tables = new HashMap<>();

  public SettingsProvider() {
    for (String table : new String[] {"global", "secure", "system", "config"}) tables.put(table, new HashMap<>());
    // A device past its setup wizard, as an application expects to find it.
    tables.get("global").put("device_provisioned", "1");
    tables.get("secure").put("user_setup_complete", "1");
    tables.get("system").put("font_scale", "1.0");
  }

  @Override
  public Bundle call(String method, String arg, Bundle extras) {
    int sep = method.indexOf('_');
    Map<String, String> table = sep < 0 ? null : tables.get(method.substring(sep + 1));
    if (table == null) return null;
    String verb = method.substring(0, sep);
    Bundle out = new Bundle();
    synchronized (tables) {
      switch (verb) {
        case "GET":
          out.putString("value", table.get(arg));
          return out;
        case "PUT":
          table.put(arg, extras == null ? null : extras.getString("value"));
          return out;
        case "DELETE":
          table.remove(arg);
          return out;
        case "LIST":
          if (!method.equals("LIST_config")) return null;
          String prefix = extras == null ? null : extras.getString("_prefix");
          HashMap<String, String> found = new HashMap<>();
          for (Map.Entry<String, String> e : table.entrySet()) {
            if (prefix == null || e.getKey().startsWith(prefix)) found.put(e.getKey(), e.getValue());
          }
          out.putSerializable("value", found);
          return out;
        default:
          return null;
      }
    }
  }

  @Override
  public boolean onCreate() {
    return true;
  }

  @Override
  public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
    return null;
  }

  @Override
  public String getType(Uri uri) {
    return null;
  }

  @Override
  public Uri insert(Uri uri, ContentValues values) {
    return null;
  }

  @Override
  public int delete(Uri uri, String selection, String[] selectionArgs) {
    return 0;
  }

  @Override
  public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
    return 0;
  }
}
