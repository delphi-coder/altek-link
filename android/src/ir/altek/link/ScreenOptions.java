package ir.altek.link;

import android.content.Context;
import android.content.SharedPreferences;

/** Screen-share encoder options, independent from camera settings. */
final class ScreenOptions {
  int maxSide = 1280, fps = 24, bitrate = 6000000, keySeconds = 2;

  static boolean valid(int maxSide, int fps, int kbps, int keySeconds) {
    return maxSide >= 320 && maxSide <= 1920 && fps >= 1 && fps <= 60
        && kbps >= 100 && kbps <= 20000 && keySeconds >= 1 && keySeconds <= 10;
  }

  static ScreenOptions load(Context context) {
    SharedPreferences p = context.getSharedPreferences("screen_video", 0);
    ScreenOptions o = new ScreenOptions();
    int max = p.getInt("max_side", 1280), fps = p.getInt("fps", 24);
    int kbps = p.getInt("kbps", 6000), key = p.getInt("key_seconds", 2);
    if (valid(max, fps, kbps, key)) {
      o.maxSide = max; o.fps = fps; o.bitrate = kbps * 1000; o.keySeconds = key;
    }
    return o;
  }

  void save(Context context) {
    context.getSharedPreferences("screen_video", 0).edit().putInt("max_side", maxSide)
        .putInt("fps", fps).putInt("kbps", bitrate / 1000)
        .putInt("key_seconds", keySeconds).apply();
  }
}
