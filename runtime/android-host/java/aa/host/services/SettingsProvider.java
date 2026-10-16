package aa.host.services;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import java.io.DataInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.HashMap;
import java.util.Map;
import java.util.Properties;

/**
 * The "settings" content provider (ADR-006 J3-J4): Settings.Global, Secure, System and the
 * DeviceConfig flags (Config), through the call() protocol of the platform's SettingsProvider
 * (GET_, PUT_, DELETE_ and LIST_config). Values not set read as null, so the framework's
 * defaults apply. Each table persists in /data/system/settings_<table>.properties. ANDROID_ID
 * (Settings.Secure "android_id") is 64 random bits drawn on first use, as the per-application
 * SSAID of Android 8+, never derived from the host. There is no generation tracking (callers
 * then read again each time).
 */
public final class SettingsProvider extends ContentProvider {
  /** Settings.AUTHORITY. */
  public static final String AUTHORITY = "settings";

  private static final String DIR = "/data/system";

  private final Map<String, Map<String, String>> tables = new HashMap<>();

  public SettingsProvider() {
    for (String table : new String[] {"global", "secure", "system", "config"}) tables.put(table, load(table));
    // A device past its setup wizard, as an application expects to find it.
    tables.get("global").putIfAbsent("device_provisioned", "1");
    tables.get("secure").putIfAbsent("user_setup_complete", "1");
    tables.get("system").putIfAbsent("font_scale", "1.0");
  }

  private static File file(String table) {
    return new File(DIR, "settings_" + table + ".properties");
  }

  private static Map<String, String> load(String table) {
    Map<String, String> values = new HashMap<>();
    File f = file(table);
    if (!f.exists()) return values;
    Properties p = new Properties();
    try (InputStream in = new FileInputStream(f)) {
      p.load(in);
    } catch (IOException e) {
      return values;  // unreadable: the defaults
    }
    for (String name : p.stringPropertyNames()) values.put(name, p.getProperty(name));
    return values;
  }

  /** Writes a table (a null value is not stored: it reads as null anyway). */
  private void save(String table) {
    Properties p = new Properties();
    for (Map.Entry<String, String> e : tables.get(table).entrySet()) {
      if (e.getValue() != null) p.setProperty(e.getKey(), e.getValue());
    }
    try (OutputStream out = new FileOutputStream(file(table))) {
      p.store(out, null);
    } catch (IOException e) {
      // No persistence (no /data/system): the values stay in memory.
    }
  }

  /** Settings.Secure.ANDROID_ID: 16 lowercase hexadecimal digits, drawn once. */
  private String androidId() {
    Map<String, String> secure = tables.get("secure");
    String id = secure.get("android_id");
    if (id != null) return id;
    byte[] bits = new byte[8];
    try (DataInputStream in = new DataInputStream(new FileInputStream("/dev/urandom"))) {
      in.readFully(bits);
    } catch (IOException e) {
      throw new IllegalStateException("[AnyAndroid] /dev/urandom illisible", e);
    }
    StringBuilder hex = new StringBuilder();
    for (byte b : bits) hex.append(String.format("%02x", b & 0xff));
    id = hex.toString();
    secure.put("android_id", id);
    save("secure");
    return id;
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
          boolean ssaid = method.equals("GET_secure") && "android_id".equals(arg);
          out.putString("value", ssaid ? androidId() : table.get(arg));
          return out;
        case "PUT":
          table.put(arg, extras == null ? null : extras.getString("value"));
          save(method.substring(sep + 1));
          return out;
        case "DELETE":
          table.remove(arg);
          save(method.substring(sep + 1));
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
