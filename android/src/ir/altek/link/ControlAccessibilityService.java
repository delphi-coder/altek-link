package ir.altek.link;

import android.accessibilityservice.AccessibilityService;
import android.accessibilityservice.GestureDescription;
import android.annotation.TargetApi;
import android.content.Context;
import android.graphics.Path;
import android.os.Build;
import android.os.Bundle;
import android.text.InputType;
import android.view.accessibility.AccessibilityNodeInfo;
import android.provider.Settings;
import android.view.accessibility.AccessibilityEvent;

/**
 * Lets this app inject taps and swipes without root, on API 24+ (dispatchGesture did not exist
 * before Android 7.0). The person has to turn this on once, by hand, in Settings > Accessibility -
 * there is no way around that step, by Android design. {@link RemoteControl} checks {@link
 * #isEnabled} before using this and falls back to root otherwise, so everything still works on a
 * phone where this was never turned on, just via root instead.
 *
 * <p>During an authorized remote-control session this service can inspect the focused editable
 * field to apply keyboard input, in addition to dispatching requested gestures.
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

  /** Insert Unicode text produced by the controller's keyboard layout. */
  public static boolean insertText(String text) {
    return edit(0, false, false, false, text);
  }

  /** Apply one PC key to the focused editable field, without root. */
  @TargetApi(21)
  public static boolean editKey(int vk, boolean shift, boolean control, boolean caps) {
    return edit(vk, shift, control, caps, null);
  }

  @TargetApi(21)
  private static boolean edit(int vk, boolean shift, boolean control, boolean caps, String input) {
    ControlAccessibilityService service = instance;
    if (service == null || Build.VERSION.SDK_INT < 21) return false;
    AccessibilityNodeInfo node = service.findFocus(AccessibilityNodeInfo.FOCUS_INPUT);
    if (node == null) return false;
    try {
      // Do not read or replace protected values; some apps expose masked text here.
      if (!node.isEditable() || node.isPassword()) return false;
      node.refresh();
      CharSequence value = node.getText();
      String text = value == null ? "" : value.toString();
      if (text.length() > 100000) return false;
      int start = node.getTextSelectionStart(), end = node.getTextSelectionEnd();
      // Before API 26, Samsung's recipient search can expose its hint as text,
      // with no actual text selection. Restrict this workaround to that field.
      boolean hint = Build.VERSION.SDK_INT >= 26 && node.isShowingHintText();
      if (Build.VERSION.SDK_INT < 26 &&
          "com.samsung.android.messaging".contentEquals(node.getPackageName()) &&
          "android:id/search_src_text".equals(node.getViewIdResourceName()) &&
          ((start < 0 && end < 0) ||
           (start == 0 && end == 0 &&
            "Search Contacts or enter number".equalsIgnoreCase(text.trim())))) hint = true;
      if (hint) { text = ""; start = end = 0; }
      if (start < 0 || end < 0 || start > text.length() || end > text.length()) start = end = text.length();
      if (start > end) { int swap = start; start = end; end = swap; }
      if (input != null) {
        String updated = text.substring(0, start) + input + text.substring(end);
        Bundle args = new Bundle();
        args.putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, updated);
        if (!node.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, args)) return false;
        select(node, start + input.length(), start + input.length());
        return true;
      }
      if (control && vk == 0x41) return select(node, 0, text.length());
      if (control) return false;
      if (vk == 0x25 || vk == 0x27 || vk == 0x24 || vk == 0x23) {
        int at = vk == 0x24 ? 0 : vk == 0x23 ? text.length() :
            vk == 0x25 ? (start != end ? start : Math.max(0, start - 1)) :
            (start != end ? end : Math.min(text.length(), end + 1));
        return select(node, at, at);
      }
      if (vk == 0x08 && start == end && start > 0)
        start = text.offsetByCodePoints(start, -1);
      else if (vk == 0x2e && start == end && end < text.length())
        end = text.offsetByCodePoints(end, 1);
      else if (vk == 0x08 || vk == 0x2e) {
        if (start == end) return true;
      }
      String insert;
      if (vk == 0x08 || vk == 0x2e) insert = "";
      else if (vk == 0x0d) {
        if ((node.getInputType() & InputType.TYPE_TEXT_FLAG_MULTI_LINE) == 0) return false;
        insert = "\n";
      } else {
        insert = RemoteKeyText.character(vk, shift, caps);
        if (insert == null) return false;
      }
      String updated = text.substring(0, start) + insert + text.substring(end);
      Bundle args = new Bundle();
      args.putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, updated);
      if (!node.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, args)) return false;
      select(node, start + insert.length(), start + insert.length());
      return true;
    } finally {
      node.recycle();
    }
  }

  @TargetApi(18)
  private static boolean select(AccessibilityNodeInfo node, int start, int end) {
    Bundle args = new Bundle();
    args.putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_START_INT, start);
    args.putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_END_INT, end);
    return node.performAction(AccessibilityNodeInfo.ACTION_SET_SELECTION, args);
  }

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
