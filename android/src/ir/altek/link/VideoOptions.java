package ir.altek.link;

import android.content.*;

/** Per camera-ID settings. Empty size/range/focus preserve the tested driver-default path. */
public final class VideoOptions {
  public String size = "", range = "", focus = "";
  public int fps = 15, bitrate = 4000000, keySeconds = 1;

  public static VideoOptions load(Context c, int id) {
    SharedPreferences p = c.getSharedPreferences("video", 0);
    String k = "camera" + id + ".";
    VideoOptions o = new VideoOptions();
    o.size = p.getString(k + "size", "");
    o.range = p.getString(k + "range", "");
    o.focus = p.getString(k + "focus", "");
    o.fps = p.getInt(k + "fps", 15);
    o.bitrate = p.getInt(k + "bitrate", 4000000);
    o.keySeconds = p.getInt(k + "key", 1);
    return o;
  }

  public void save(Context c, int id) {
    String k = "camera" + id + ".";
    c.getSharedPreferences("video", 0)
        .edit()
        .putString(k + "size", size)
        .putString(k + "range", range)
        .putString(k + "focus", focus)
        .putInt(k + "fps", fps)
        .putInt(k + "bitrate", bitrate)
        .putInt(k + "key", keySeconds)
        .apply();
  }

  public boolean isDefault() {
    return size.length() == 0
        && range.length() == 0
        && focus.length() == 0
        && fps == 15
        && bitrate == 4000000
        && keySeconds == 1;
  }

  public static boolean valid(int fps, int kbps, int key) {
    return fps >= 1 && fps <= 120 && kbps >= 32 && kbps <= 20000 && key >= 1 && key <= 10;
  }

  public static long periodNs(int fps) {
    return 1000000000L / Math.max(1, fps);
  }
}
