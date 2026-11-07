package ir.altek.link;

import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Executes gestures received from the controlling phone, picking the best available method:
 *
 * <ul>
 *   <li>{@link ControlAccessibilityService}, if it is turned on (API 24+ only - no root).
 *   <li>Otherwise, root via `su -c input ...` - the only option below API 24, and the fallback
 *       above it when the person has not turned the accessibility service on.
 * </ul>
 *
 * Coordinates arrive normalized (0..1) so they are independent of whatever resolution the screen is
 * being streamed at; they are scaled here to this device's real screen size.
 *
 * <p>Root commands run one at a time on a single background thread. If gestures arrive faster than
 * they can run, the newest ones are dropped rather than queued, so input never lags behind by
 * replaying stale taps after the user has moved on. The accessibility path is not queued at all:
 * dispatchGesture() is effectively instant to hand off, so there is nothing to queue there.
 */
public final class RemoteControl {
  private static final int MAX_PENDING = 2;

  private final int width, height;
  private final ExecutorService exec = Executors.newSingleThreadExecutor();
  private final AtomicInteger pending = new AtomicInteger();
  private final ThreadPoolExecutor keys = new ThreadPoolExecutor(
      1, 1, 0, TimeUnit.MILLISECONDS, new ArrayBlockingQueue<Runnable>(128));
  private boolean shiftDown, ctrlDown, capsLock;
  private volatile boolean stopped;

  public RemoteControl(int width, int height) {
    this.width = Math.max(1, width);
    this.height = Math.max(1, height);
  }

  /** True if gestures will be injected without root. Used only to tell the person what to expect. */
  public boolean rootFree() {
    return ControlAccessibilityService.isEnabled();
  }

  /** action is one of AccessibilityService.GLOBAL_ACTION_BACK/HOME/RECENTS. */
  public void key(int action) {
    // performGlobalAction() works on API 16+ (unlike dispatchGesture, which needs API 24), so this
    // can go root-free on older phones too whenever the accessibility service is turned on.
    if (ControlAccessibilityService.enabled() && ControlAccessibilityService.globalAction(action)) {
      return;
    }
    int keycode =
        action == 1 /* GLOBAL_ACTION_BACK */
            ? 4
            : action == 2 /* GLOBAL_ACTION_HOME */ ? 3 : 187 /* GLOBAL_ACTION_RECENTS: APP_SWITCH */;
    submit("input keyevent " + keycode);
  }

  public void tap(float nx, float ny) {
    float x = px(nx), y = py(ny);
    if (ControlAccessibilityService.isEnabled() && ControlAccessibilityService.tap(x, y)) return;
    submit("input tap " + Math.round(x) + " " + Math.round(y));
  }

  public void swipe(float nx1, float ny1, float nx2, float ny2, int durationMs) {
    if (durationMs < 1) durationMs = 1;
    if (durationMs > 10000) durationMs = 10000;
    float x1 = px(nx1), y1 = py(ny1), x2 = px(nx2), y2 = py(ny2);
    if (ControlAccessibilityService.isEnabled()
        && ControlAccessibilityService.swipe(x1, y1, x2, y2, durationMs)) return;
    submit(
        "input swipe "
            + Math.round(x1)
            + " "
            + Math.round(y1)
            + " "
            + Math.round(x2)
            + " "
            + Math.round(y2)
            + " "
            + durationMs);
  }

  /**
   * Multiplicative pinch: scale>1 zooms in, scale<1 zooms out. Has no root fallback: pinch is only
   * truly possible with the accessibility service (dispatchGesture with two strokes). Without it we
   * log and do nothing rather than faking a random swipe.
   */
  public void pinch(float nx, float ny, float scale) {
    if (scale <= 0) return;
    DebugLog.event("REMOTE_PINCH", "nx=" + nx + " ny=" + ny + " scale=" + scale);
    if (ControlAccessibilityService.isEnabled()
        && ControlAccessibilityService.pinch(px(nx), py(ny), scale)) return;
    DebugLog.event("REMOTE_INPUT", "pinch needs accessibility service nx=" + nx + " ny=" + ny);
  }

  /** position of the last mouseDown for the up-event heuristic. */
  private float downNx = -1, downNy = -1;
  private long downTime;

  /** Raw mouse down: remember the origin so the following up can be matched. */
  public void mouseDown(float nx, float ny) {
    downNx = nx;
    downNy = ny;
    downTime = System.nanoTime() / 1000000L;
  }

  /** Raw mouse move: no-op on the root fallback (input swipe is discrete);
   *  when the accessibility service is on, we could in theory stream a
   *  GestureDescription, but for v1 tap/swipe already covers the workflow. */
  public void mouseMove(float nx, float ny) {}

  /** Raw mouse up: decide tap vs swipe from how far the finger travelled. */
  public void mouseUp(float nx, float ny) {
    if (downNx < 0) {
      tap(nx, ny);
      return;
    }
    float dx = px(nx) - px(downNx), dy = py(ny) - py(downNy);
    double dist = Math.sqrt(dx * dx + dy * dy);
    int durationMs = (int) (System.nanoTime() / 1000000L - downTime);
    if (dist < 24) {
      tap(nx, ny);
    } else {
      swipe(downNx, downNy, nx, ny, Math.max(50, durationMs));
    }
    downNx = -1;
  }

  /** Right-click from the PC controller: Android has no right button, so the
   *  analogue is a long-press at the same point. */
  private float rightNx = -1, rightNy = -1;

  public void rightDown(float nx, float ny) {
    rightNx = nx;
    rightNy = ny;
  }

  public void rightUp(float nx, float ny) {
    if (rightNx >= 0) {
      longPress(rightNx, rightNy);
    } else {
      longPress(nx, ny);
    }
    rightNx = -1;
  }

  /** Long-press at a point: hold for 600ms without moving. */
  public void longPress(float nx, float ny) {
    float x = px(nx), y = py(ny);
    if (ControlAccessibilityService.isEnabled() && ControlAccessibilityService.press(x, y)) return;
    submit("input swipe " + Math.round(x) + " " + Math.round(y) + " " + Math.round(x) + " " + Math.round(y) + " 600");
  }

  /**
   * Forwarded hardware key from a PC controller. Root `input keyevent` is
   * atomic (press+release), so we only fire it on key-down; the up half is
   * produced by the same command. Keycode mapping is PC VK -> Android.
   */
  public void keyEvent(final int vk, int down) {
    if (vk == 0x10 || vk == 0xa0 || vk == 0xa1) { shiftDown = down != 0; return; }
    if (vk == 0x11 || vk == 0xa2 || vk == 0xa3) { ctrlDown = down != 0; return; }
    if (down == 0) return;
    if (vk == 0x14) { capsLock = !capsLock; return; }
    final int code = vkToAndroid(vk);
    if (code <= 0 || stopped) return;
    final boolean shift = shiftDown, control = ctrlDown, caps = capsLock;
    try {
      keys.execute(new Runnable() {
        public void run() {
          if (stopped) return;
          try {
            if (ControlAccessibilityService.editKey(vk, shift, control, caps)) return;
            // Root keyevent is an atomic key press; it cannot preserve Ctrl combinations.
            if (!control) RootShell.run("input keyevent " + code, 4000);
          } catch (Exception e) {
            DebugLog.error("REMOTE_KEY", e);
          }
        }
      });
    } catch (RejectedExecutionException e) {
      DebugLog.event("REMOTE_KEY", "keyboard queue full");
    }
  }

  public void textInput(final String text) {
    if (stopped || text == null || text.length() == 0) return;
    try {
      keys.execute(new Runnable() {
        public void run() {
          if (!stopped) ControlAccessibilityService.insertText(text);
        }
      });
    } catch (RejectedExecutionException e) {
      DebugLog.event("REMOTE_KEY", "keyboard queue full");
    }
  }

  private static int vkToAndroid(int vk) {
    if (vk >= 0x41 && vk <= 0x5A) return 29 + (vk - 0x41); // A..Z
    if (vk >= 0x30 && vk <= 0x39) return 7 + (vk - 0x30); // 0..9
    if (vk >= 0x60 && vk <= 0x69) return 7 + (vk - 0x60); // numpad = digits
    if (vk >= 0x70 && vk <= 0x7B) return 131 + (vk - 0x70); // F1..F12
    switch (vk) {
      case 0x08: return 67;   // VK_BACK -> DEL (backspace)
      case 0x09: return 61;   // VK_TAB
      case 0x0D: return 66;   // VK_RETURN
      case 0x10: return 59;   // VK_SHIFT
      case 0x11: return 113;  // VK_CONTROL
      case 0x12: return 57;   // VK_MENU -> ALT
      case 0x14: return 115;  // VK_CAPITAL -> CAPS_LOCK
      case 0x1B: return 111;  // VK_ESCAPE
      case 0x20: return 62;   // VK_SPACE
      case 0x21: return 92;   // VK_PRIOR -> PAGE_UP
      case 0x22: return 93;   // VK_NEXT -> PAGE_DOWN
      case 0x23: return 123;  // VK_END -> MOVE_END
      case 0x24: return 122;  // VK_HOME -> MOVE_HOME
      case 0x25: return 21;   // VK_LEFT
      case 0x26: return 19;   // VK_UP
      case 0x27: return 22;   // VK_RIGHT
      case 0x28: return 20;   // VK_DOWN
      case 0x2D: return 124;  // VK_INSERT
      case 0x2E: return 112;  // VK_DELETE -> FORWARD_DELETE
      case 0xBA: return 74;   // VK_OEM_1 -> SEMICOLON
      case 0xBB: return 70;   // VK_OEM_PLUS -> EQUALS
      case 0xBC: return 55;   // VK_OEM_COMMA
      case 0xBD: return 69;   // VK_OEM_MINUS
      case 0xBE: return 56;   // VK_OEM_PERIOD
      case 0xBF: return 76;   // VK_OEM_2 -> SLASH
      case 0xC0: return 68;   // VK_OEM_3 -> GRAVE
      case 0xDB: return 71;   // VK_OEM_4 -> LEFT_BRACKET
      case 0xDC: return 73;   // VK_OEM_5 -> BACKSLASH
      case 0xDD: return 72;   // VK_OEM_6 -> RIGHT_BRACKET
      case 0xDE: return 75;   // VK_OEM_7 -> APOSTROPHE
      default:
        return 0;
    }
  }

  private float px(float n) {
    return clamp(n * width, 0, width - 1);
  }

  private float py(float n) {
    return clamp(n * height, 0, height - 1);
  }

  private static float clamp(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
  }

  private void submit(final String shellCommand) {
    if (stopped || pending.get() > MAX_PENDING) return;
    pending.incrementAndGet();
    try {
      exec.execute(
          new Runnable() {
            public void run() {
              try {
                RootShell.run(shellCommand, 4000);
              } catch (Exception e) {
                DebugLog.error("REMOTE_INPUT", e);
              } finally {
                pending.decrementAndGet();
              }
            }
          });
    } catch (RejectedExecutionException e) {
      pending.decrementAndGet();
    }
  }

  public void stop() {
    stopped = true;
    exec.shutdownNow();
    keys.shutdownNow();
  }
}
