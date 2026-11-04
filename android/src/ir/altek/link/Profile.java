package ir.altek.link;

import android.content.*;
import android.graphics.*;
import android.util.Base64;
import android.util.LruCache;
import java.io.*;
import org.json.JSONObject;

/** Small, content-addressed profile images. No image bytes in discovery packets. */
public final class Profile {
  private static final LruCache<String, Bitmap> memory = new LruCache<String, Bitmap>(24);

  private static SharedPreferences prefs(Context c) {
    return c.getSharedPreferences("profiles", 0);
  }

  private static File file(Context c, String hash) {
    return new File(c.getFilesDir(), "avatar_" + hash + ".jpg");
  }

  public static String hash(Context c, String peer) {
    return prefs(c).getString(peer, "");
  }

  public static boolean has(Context c, String hash) {
    return hash.length() == 0 || (hash.matches("[0-9a-f]{64}") && file(c, hash).isFile());
  }

  public static synchronized void remove(Context c) {
    prefs(c).edit().putString("self", "").commit();
    cleanup(c);
    changed();
  }

  private static void changed() {
    LinkService s = LinkService.current;
    if (s != null) {
      s.change();
      s.profileChanged();
    }
  }

  public static synchronized void save(Context c, Bitmap bitmap) throws Exception {
    ByteArrayOutputStream out = new ByteArrayOutputStream();
    for (int quality = 85; quality >= 25; quality -= 15) {
      out.reset();
      bitmap.compress(Bitmap.CompressFormat.JPEG, quality, out);
      if (out.size() <= 40000) break;
    }
    if (out.size() > 40000) throw new IOException("Profile too large");
    byte[] bytes = out.toByteArray();
    String hash = Wire.hex(java.security.MessageDigest.getInstance("SHA-256").digest(bytes));
    put(c, hash, bytes);
    prefs(c).edit().putString("self", hash).commit();
    cleanup(c);
    changed();
  }

  private static void put(Context c, String hash, byte[] bytes) throws Exception {
    File dest = file(c, hash), tmp = new File(c.getFilesDir(), "avatar-" + Wire.uid() + ".tmp");
    try {
      FileOutputStream out = new FileOutputStream(tmp);
      try {
        out.write(bytes);
      } finally {
        out.close();
      }
      if (!tmp.renameTo(dest)) throw new IOException("Profile write failed");
    } finally {
      tmp.delete();
    }
  }

  public static synchronized JSONObject response(Context c, String known) throws Exception {
    String hash = hash(c, "self");
    JSONObject j = new JSONObject().put("ok", true).put("hash", hash);
    if (hash.length() > 0 && !hash.equals(known)) {
      FileInputStream in = new FileInputStream(file(c, hash));
      ByteArrayOutputStream out = new ByteArrayOutputStream();
      try {
        byte[] b = new byte[4096];
        int n;
        while ((n = in.read(b)) != -1) out.write(b, 0, n);
      } finally {
        in.close();
      }
      j.put("image", Base64.encodeToString(out.toByteArray(), Base64.NO_WRAP));
    }
    return j;
  }

  public static synchronized void accept(Context c, String peer, JSONObject j) throws Exception {
    String hash = j.getString("hash");
    if (hash.length() > 0) {
      if (!hash.matches("[0-9a-f]{64}")) throw new IOException("Profile hash");
      if (!has(c, hash)) {
        byte[] b = Base64.decode(j.getString("image"), Base64.DEFAULT);
        if (b.length > 40000
            || !hash.equals(Wire.hex(java.security.MessageDigest.getInstance("SHA-256").digest(b))))
          throw new IOException("Profile checksum");
        BitmapFactory.Options opts = new BitmapFactory.Options();
        opts.inJustDecodeBounds = true;
        BitmapFactory.decodeByteArray(b, 0, b.length, opts);
        if (opts.outWidth != 256 || opts.outHeight != 256)
          throw new IOException("Profile dimensions");
        put(c, hash, b);
      }
    }
    prefs(c).edit().putString(peer, hash).commit();
    cleanup(c);
  }

  private static void cleanup(Context c) {
    java.util.HashSet<String> keep = new java.util.HashSet<String>();
    for (Object value : prefs(c).getAll().values())
      if (value instanceof String) keep.add("avatar_" + value + ".jpg");
    File[] files = c.getFilesDir().listFiles();
    if (files != null)
      for (File f : files)
        if (f.getName().matches("avatar_[0-9a-f]{64}\\.jpg") && !keep.contains(f.getName()))
          f.delete();
  }

  public static Bitmap bitmap(Context c, String peer) {
    String hash = hash(c, peer);
    if (hash.length() == 0) return null;
    synchronized (memory) {
      Bitmap b = memory.get(hash);
      if (b == null) {
        b = BitmapFactory.decodeFile(file(c, hash).getPath());
        if (b != null) memory.put(hash, b);
      }
      return b;
    }
  }
}
