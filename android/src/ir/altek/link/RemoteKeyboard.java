package ir.altek.link;

import android.content.Context;
import android.os.Handler;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import java.util.ArrayDeque;

/** Bridges the selected Android IME to text-capable peers and legacy virtual-key peers. */
final class RemoteKeyboard extends View {
  interface Target {
    void key(int vk, boolean down);
    boolean acceptsText();
    void text(String text);
    void unsupported();
    void hidden();
  }

  private final Target target;
  private final Handler handler = new Handler();
  private final ArrayDeque<Object> pending = new ArrayDeque<Object>();
  private boolean scheduled, closed, warned;
  private String composing = "";
  private final Runnable sendNext = new Runnable() {
    public void run() {
      if (closed) return;
      Object event = pending.pollFirst();
      if (event == null) { scheduled = false; return; }
      if (event instanceof String) target.text((String) event);
      else {
        int key = ((Integer) event).intValue();
        target.key(key & 255, (key & 256) != 0);
      }
      handler.postDelayed(this, 6);
    }
  };

  RemoteKeyboard(Context context, Target target) {
    super(context);
    this.target = target;
    setFocusable(true);
    setFocusableInTouchMode(true);
  }

  public boolean onCheckIsTextEditor() { return true; }

  public InputConnection onCreateInputConnection(EditorInfo out) {
    out.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
    out.imeOptions = EditorInfo.IME_ACTION_NONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI;
    return new BaseInputConnection(this, false) {
      public boolean setComposingText(CharSequence text, int cursor) {
        replaceComposition(text == null ? "" : text.toString(), true);
        return true;
      }
      public boolean commitText(CharSequence text, int cursor) {
        replaceComposition(text == null ? "" : text.toString(), false);
        return true;
      }
      public boolean finishComposingText() { composing = ""; return true; }
      public boolean deleteSurroundingText(int before, int after) {
        int n = Math.min(Math.max(before, 0), 200);
        while (n-- > 0) {
          press(0x08);
          if (composing.length() > 0)
            composing = composing.substring(0, composing.offsetByCodePoints(composing.length(), -1));
        }
        n = Math.min(Math.max(after, 0), 200);
        while (n-- > 0) press(0x2e);
        return true;
      }
      public boolean deleteSurroundingTextInCodePoints(int before, int after) {
        return deleteSurroundingText(before, after);
      }
      public boolean sendKeyEvent(KeyEvent event) { return forward(event); }
      public boolean performEditorAction(int action) { press(0x0d); composing = ""; return true; }
    };
  }

  private void replaceComposition(String input, boolean keep) {
    StringBuilder supported = new StringBuilder();
    boolean textMode = target.acceptsText();
    for (int i = 0; i < input.length(); ) {
      int cp = input.codePointAt(i);
      i += Character.charCount(cp);
      if (textMode || (cp <= 0xffff && encodedCharacter((char) cp) >= 0))
        supported.appendCodePoint(cp);
      else if (!warned) { warned = true; target.unsupported(); }
    }
    String text = supported.toString();
    int prefix = 0;
    while (prefix < composing.length() && prefix < text.length()) {
      int previous = composing.codePointAt(prefix), next = text.codePointAt(prefix);
      if (previous != next) break;
      prefix += Character.charCount(previous);
    }
    int remove = composing.codePointCount(prefix, composing.length());
    while (remove-- > 0) press(0x08);
    for (int i = prefix; i < text.length(); ) {
      int cp = text.codePointAt(i);
      int count = Character.charCount(cp);
      if (cp == '\n') press(0x0d);
      else if (cp == '\t') press(0x09);
      else if (textMode) enqueueText(text.substring(i, i + count));
      else type((char) cp);
      i += count;
    }
    composing = keep ? text : "";
  }

  private void type(char c) {
    int code = encodedCharacter(c);
    if (code < 0) return;
    if ((code & 256) != 0) enqueue(0x10, true);
    press(code & 255);
    if ((code & 256) != 0) enqueue(0x10, false);
  }

  /* US physical-key positions. The target Windows keyboard layout controls the result. */
  static int encodedCharacter(char c) {
    if (c >= 'a' && c <= 'z') return c - 'a' + 0x41;
    if (c >= 'A' && c <= 'Z') return (c - 'A' + 0x41) | 256;
    if (c >= '0' && c <= '9') return c;
    if (c == ' ') return 0x20;
    if (c == '\n') return 0x0d;
    if (c == '\t') return 0x09;
    String normal = ";=,-./`[\\]'";
    String shifted = ":+<_>?~{|}\"";
    int[] keys = {0xba,0xbb,0xbc,0xbd,0xbe,0xbf,0xc0,0xdb,0xdc,0xdd,0xde};
    int i = normal.indexOf(c);
    if (i >= 0) return keys[i];
    i = shifted.indexOf(c);
    if (i >= 0) return keys[i] | 256;
    i = ")!@#$%^&*(".indexOf(c);
    return i >= 0 ? (0x30 + i) | 256 : -1;
  }

  private boolean forward(KeyEvent event) {
    int action = event.getAction();
    if (action != KeyEvent.ACTION_DOWN && action != KeyEvent.ACTION_UP) return false;
    int vk = virtualKey(event.getKeyCode());
    if (vk == 0) return false;
    if (action == KeyEvent.ACTION_DOWN && event.getRepeatCount() > 0) {
      enqueue(vk, false);
    }
    enqueue(vk, action == KeyEvent.ACTION_DOWN);
    if (vk != 0x10 && vk != 0x11 && vk != 0x12) composing = "";
    return true;
  }

  public boolean onKeyDown(int code, KeyEvent event) { return forward(event) || super.onKeyDown(code, event); }
  public boolean onKeyUp(int code, KeyEvent event) { return forward(event) || super.onKeyUp(code, event); }
  public boolean onKeyPreIme(int code, KeyEvent event) {
    if (code == KeyEvent.KEYCODE_BACK) {
      if (event.getAction() == KeyEvent.ACTION_UP) target.hidden();
      return true;
    }
    return super.onKeyPreIme(code, event);
  }

  static int virtualKey(int key) {
    if (key >= KeyEvent.KEYCODE_A && key <= KeyEvent.KEYCODE_Z) return 0x41 + key - KeyEvent.KEYCODE_A;
    if (key >= KeyEvent.KEYCODE_0 && key <= KeyEvent.KEYCODE_9) return 0x30 + key - KeyEvent.KEYCODE_0;
    if (key >= KeyEvent.KEYCODE_F1 && key <= KeyEvent.KEYCODE_F12) return 0x70 + key - KeyEvent.KEYCODE_F1;
    switch (key) {
      case KeyEvent.KEYCODE_DEL: return 0x08;
      case KeyEvent.KEYCODE_TAB: return 0x09;
      case KeyEvent.KEYCODE_ENTER: return 0x0d;
      case KeyEvent.KEYCODE_SHIFT_LEFT: case KeyEvent.KEYCODE_SHIFT_RIGHT: return 0x10;
      case KeyEvent.KEYCODE_CTRL_LEFT: case KeyEvent.KEYCODE_CTRL_RIGHT: return 0x11;
      case KeyEvent.KEYCODE_ALT_LEFT: case KeyEvent.KEYCODE_ALT_RIGHT: return 0x12;
      case KeyEvent.KEYCODE_CAPS_LOCK: return 0x14;
      case KeyEvent.KEYCODE_ESCAPE: return 0x1b;
      case KeyEvent.KEYCODE_SPACE: return 0x20;
      case KeyEvent.KEYCODE_PAGE_UP: return 0x21;
      case KeyEvent.KEYCODE_PAGE_DOWN: return 0x22;
      case KeyEvent.KEYCODE_MOVE_END: return 0x23;
      case KeyEvent.KEYCODE_MOVE_HOME: return 0x24;
      case KeyEvent.KEYCODE_DPAD_LEFT: return 0x25;
      case KeyEvent.KEYCODE_DPAD_UP: return 0x26;
      case KeyEvent.KEYCODE_DPAD_RIGHT: return 0x27;
      case KeyEvent.KEYCODE_DPAD_DOWN: return 0x28;
      case KeyEvent.KEYCODE_INSERT: return 0x2d;
      case KeyEvent.KEYCODE_FORWARD_DEL: return 0x2e;
      case KeyEvent.KEYCODE_META_LEFT: return 0x5b;
      case KeyEvent.KEYCODE_META_RIGHT: return 0x5c;
      case KeyEvent.KEYCODE_SEMICOLON: return 0xba;
      case KeyEvent.KEYCODE_EQUALS: return 0xbb;
      case KeyEvent.KEYCODE_COMMA: return 0xbc;
      case KeyEvent.KEYCODE_MINUS: return 0xbd;
      case KeyEvent.KEYCODE_PERIOD: return 0xbe;
      case KeyEvent.KEYCODE_SLASH: return 0xbf;
      case KeyEvent.KEYCODE_GRAVE: return 0xc0;
      case KeyEvent.KEYCODE_LEFT_BRACKET: return 0xdb;
      case KeyEvent.KEYCODE_BACKSLASH: return 0xdc;
      case KeyEvent.KEYCODE_RIGHT_BRACKET: return 0xdd;
      case KeyEvent.KEYCODE_APOSTROPHE: return 0xde;
      default: return 0;
    }
  }

  private void press(int vk) { enqueue(vk, true); enqueue(vk, false); }
  private void enqueueText(String text) { enqueueEvent(text); }
  private void enqueue(int vk, boolean down) {
    enqueueEvent(Integer.valueOf(vk | (down ? 256 : 0)));
  }
  private void enqueueEvent(Object event) {
    if (closed) return;
    if (pending.size() >= 2048) {
      if (!warned) { warned = true; target.unsupported(); }
      return;
    }
    pending.addLast(event);
    if (!scheduled) { scheduled = true; handler.post(sendNext); }
  }
  void close() { closed = true; handler.removeCallbacks(sendNext); pending.clear(); }
}
