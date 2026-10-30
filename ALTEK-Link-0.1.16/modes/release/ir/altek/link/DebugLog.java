package ir.altek.link;

import android.content.Context;

/** Release builds deliberately contain no diagnostic logging implementation. */
public final class DebugLog {
  public static void init(Context context, String identity) {}

  public static void event(String component, String message) {}

  public static void error(String component, Throwable error) {}
}
