package ir.altek.link;

import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.lang.reflect.Method;

/** Persistent API-19 input injector, run by app_process in the shell SELinux context. */
public final class RootInputHelper {
  public static void main(String[] args) throws Exception {
    Class<?> managerClass = Class.forName("android.hardware.input.InputManager");
    Object manager = managerClass.getMethod("getInstance").invoke(null);
    Method inject = managerClass.getMethod("injectInputEvent", android.view.InputEvent.class, Integer.TYPE);
    BufferedReader reader = new BufferedReader(new InputStreamReader(System.in, "UTF-8"));
    long downTime = 0;
    String line;
    while ((line = reader.readLine()) != null) {
      String[] part = line.trim().split(" +");
      if (part.length == 0 || part[0].length() == 0) continue;
      if ("X".equals(part[0])) break;
      long now = SystemClock.uptimeMillis();
      if ("K".equals(part[0]) && part.length == 2) {
        int code = Integer.parseInt(part[1]);
        inject.invoke(manager, new KeyEvent(now, now, KeyEvent.ACTION_DOWN, code, 0), 0);
        inject.invoke(manager, new KeyEvent(now, now, KeyEvent.ACTION_UP, code, 0), 0);
      } else if (("S".equals(part[0]) && part.length == 6)
          || ("L".equals(part[0]) && part.length == 3)) {
        float x1 = Float.parseFloat(part[1]), y1 = Float.parseFloat(part[2]);
        float x2 = "S".equals(part[0]) ? Float.parseFloat(part[3]) : x1;
        float y2 = "S".equals(part[0]) ? Float.parseFloat(part[4]) : y1;
        int duration = "S".equals(part[0]) ? Integer.parseInt(part[5]) : 600;
        long gestureDown = now;
        injectMotion(inject, manager, gestureDown, now, MotionEvent.ACTION_DOWN, x1, y1);
        int steps = Math.max(1, duration / 16);
        for (int i = 1; i < steps; i++) {
          SystemClock.sleep(Math.max(1, duration / steps));
          float f = (float) i / steps;
          injectMotion(inject, manager, gestureDown, SystemClock.uptimeMillis(),
              MotionEvent.ACTION_MOVE, x1 + (x2 - x1) * f, y1 + (y2 - y1) * f);
        }
        long end = gestureDown + duration;
        long wait = end - SystemClock.uptimeMillis();
        if (wait > 0) SystemClock.sleep(wait);
        injectMotion(inject, manager, gestureDown, SystemClock.uptimeMillis(),
            MotionEvent.ACTION_UP, x2, y2);
      } else if (("D".equals(part[0]) || "M".equals(part[0]) || "U".equals(part[0]))
          && part.length == 3) {
        int action;
        if ("D".equals(part[0])) {
          action = MotionEvent.ACTION_DOWN;
          downTime = now;
        } else if ("M".equals(part[0])) {
          if (downTime == 0) continue;
          action = MotionEvent.ACTION_MOVE;
        } else {
          if (downTime == 0) continue;
          action = MotionEvent.ACTION_UP;
        }
        injectMotion(inject, manager, downTime, now, action,
            Float.parseFloat(part[1]), Float.parseFloat(part[2]));
        if (action == MotionEvent.ACTION_UP) downTime = 0;
      }
    }
  }

  private static void injectMotion(Method inject, Object manager, long downTime, long eventTime,
      int action, float x, float y) throws Exception {
    MotionEvent event = MotionEvent.obtain(downTime, eventTime, action, x, y, 0);
    event.setSource(InputDevice.SOURCE_TOUCHSCREEN);
    try { inject.invoke(manager, event, 0); } finally { event.recycle(); }
  }
}
