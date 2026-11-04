package ir.altek.link;

import android.accessibilityservice.AccessibilityService;
import android.accessibilityservice.GestureDescription;
import android.annotation.TargetApi;
import android.content.Context;
import android.graphics.Path;
import android.os.Build;
import android.provider.Settings;
import android.view.accessibility.AccessibilityEvent;

/**
 * Lets this app inject taps and swipes without root, on API 24+ (dispatchGesture did not exist
 * before Android 7.0). The person has to turn this on once, by hand, in Settings > Accessibility -
 * there is no way around that step, by Android design. {@link RemoteControl} checks {@link
 * #isEnabled} before using this and falls back to root otherwise, so everything still works on a
 * phone where this was never turned on, just via root instead.
 *
 * <p>This service does not read or act on anything happening on the phone by itself; it only
 * exists to be asked, from inside this app, to play back a tap or swipe the other person sent.
 */
@TargetApi(24)
public final class ControlAccessibilityService extends AccessibilityService {
  private static volatile ControlAccessibilityService instance;

  /** True if the service is on and can inject tap/swipe gestures (dispatchGesture needs API 24). */
  public static boolean isEnabled() {
    return Build.VERSION.SDK_INT >= 24 && instance != null;
  }

  /** True if the service is on at all - performGlobalAction (Back/Home/Recents) works on API 16+. */
  public static boolean enabled() {
    return instance != null;
  }

  /** action is one of AccessibilityService.GLOBAL_ACTION_BACK/HOME/RECENTS. */
  public static boolean globalAction(int action) {
    ControlAccessibilityService s = instance;
    return s != null && s.performGlobalAction(action);
  }

  /** Opens Settings > Accessibility so the person can turn this service on. */
  public static void openSettings(Context context) {
    context.startActivity(
        new android.content.Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS)
            .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK));
  }

  protected void onServiceConnected() {
    super.onServiceConnected();
    instance = this;
  }

  public boolean onUnbind(android.content.Intent intent) {
    instance = null;
    return super.onUnbind(intent);
  }

  public void onAccessibilityEvent(AccessibilityEvent event) {}

  public void onInterrupt() {}

  /** Returns true only once the system has actually accepted the gesture for dispatch. */
  public static boolean tap(float x, float y) {
    ControlAccessibilityService s = instance;
    if (s == null) return false;
    Path p = new Path();
    p.moveTo(x, y);
    GestureDescription.Builder b = new GestureDescription.Builder();
    b.addStroke(new GestureDescription.StrokeDescription(p, 0, 40));
    return s.dispatchGesture(b.build(), null, null);
  }

  /** Long-press: hold a stroke in place for the given duration (the touch analogue of right-click). */
  public static boolean press(float x, float y) {
    ControlAccessibilityService s = instance;
    if (s == null) return false;
    Path p = new Path();
    p.moveTo(x, y);
    GestureDescription.Builder b = new GestureDescription.Builder();
    b.addStroke(new GestureDescription.StrokeDescription(p, 0, 600));
    return s.dispatchGesture(b.build(), null, null);
  }

  public static boolean swipe(float x1, float y1, float x2, float y2, int durationMs) {
    ControlAccessibilityService s = instance;
    if (s == null) return false;
    if (durationMs < 1) durationMs = 1;
    Path p = new Path();
    p.moveTo(x1, y1);
    p.lineTo(x2, y2);
    GestureDescription.Builder b = new GestureDescription.Builder();
    b.addStroke(new GestureDescription.StrokeDescription(p, 0, durationMs));
    return s.dispatchGesture(b.build(), null, null);
  }

  /**
   * Two-finger pinch: the two strokes start an equal distance from (x,y) and move in/out according to
   * `scale`. scale>1 = zoom in (fingers apart), scale<1 = zoom out (fingers together). The strokes run
   * vertically around the midpoint to look natural to Gallery-style pinch handling.
   */
  public static boolean pinch(float x, float y, float scale) {
    ControlAccessibilityService s = instance;
    if (s == null || scale <= 0) return false;
    final int d = 200; // pixel half-distance of the two fingers at gesture start
    float r1 = d;
    float r2 = d * scale;
    if (r2 < 1) r2 = 1;
    // If the movement is too short to be meaningful, skip to avoid degenerate paths.
    if (Math.abs(r2 - r1) < 2) return true;
    /* Horizontal pinch: finger at left/right of midpoint moves out/in. */
    float diff = Math.abs(r2 - r1);
    // Older/target apps sometimes only recognize the gesture if it is real enough.
    // A too-fast two-finger move is misclassified as a swipe or tap.
    int duration;
    if (diff < 10) duration = 300;
    else if (diff > 220) duration = 500;
    else duration = (int) Math.max(300f, Math.min(500f, diff * 2.0f));
    Path left = new Path();
    left.moveTo(x - r1, y);
    left.lineTo(x - r2, y);
    Path right = new Path();
    right.moveTo(x + r1, y);
    right.lineTo(x + r2, y);
    GestureDescription.Builder b = new GestureDescription.Builder();
    b.addStroke(new GestureDescription.StrokeDescription(left, 0, duration));
    b.addStroke(new GestureDescription.StrokeDescription(right, 0, duration));
    return s.dispatchGesture(b.build(), null, null);
  }
}
