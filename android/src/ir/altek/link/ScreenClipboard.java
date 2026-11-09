package ir.altek.link;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.os.Build;
import android.os.Handler;
import android.widget.Toast;

/** Per-session text clipboard, accessed only on the Android main thread. */
final class ScreenClipboard implements ClipboardManager.OnPrimaryClipChangedListener {
  private final ClipboardManager clipboard;
  private final Context context;
  private final Handler main = new Handler();
  private final ScreenEngine engine;
  private final boolean controller;
  private int mode;
  private boolean focused, closed, ignoreNextEvent;
  private String lastSent, received;
  private long lastWarning;

  ScreenClipboard(Context context, ScreenEngine engine, boolean controller) {
    this.context = context.getApplicationContext();
    this.engine = engine;
    this.controller = controller;
    clipboard = (ClipboardManager) this.context.getSystemService(Context.CLIPBOARD_SERVICE);
    clipboard.addPrimaryClipChangedListener(this);
  }
  void focus(boolean value) { if (focused == value) return; focused = value; if (value) check(false); }
  void peerMode(int value) { if (!controller) setMode(value); }
  void setMode(final int value) {
    main.post(new Runnable() { public void run() {
      if (closed) return;
      mode = value & 3;
      lastSent = null;
      // In two-way mode the controller supplies the initial clipboard. Sending
      // the remote clipboard here can overwrite a fresh copy on the controller.
      if (controller || mode != 3) check(false);
    }});
  }
  void receive(final String text) {
    main.post(new Runnable() { public void run() {
      if (closed || (mode & (controller ? 2 : 1)) == 0) return;
      received = text;
      ignoreNextEvent = true;
      clipboard.setPrimaryClip(ClipData.newPlainText("ALTEK Link", text));
    }});
  }
  public void onPrimaryClipChanged() {
    if (ignoreNextEvent) { ignoreNextEvent = false; return; }
    check(true);
  }
  private void check(boolean event) {
    if (closed || (mode & (controller ? 1 : 2)) == 0) return;
    if (Build.VERSION.SDK_INT >= 29 && !focused) {
      long now = android.os.SystemClock.elapsedRealtime();
      if (event && now - lastWarning > 5000) {
        lastWarning = now;
        Toast.makeText(context, "Open ALTEK Link to send the copied text", Toast.LENGTH_LONG).show();
      }
      return;
    }
    try {
      ClipData data = clipboard.getPrimaryClip();
      if (data == null || data.getItemCount() == 0) return;
      CharSequence value = data.getItemAt(0).getText();
      if (value == null) return;
      String text = value.toString();
      if (text.equals(received)) { received = null; return; }
      received = null;
      if (text.equals(lastSent)) return;
      if (engine.sendClipboard(text)) lastSent = text;
    } catch (SecurityException ignored) {
      if (event) Toast.makeText(context, "Open ALTEK Link to send the copied text", Toast.LENGTH_LONG).show();
    }
  }
  void close() { closed = true; clipboard.removePrimaryClipChangedListener(this); }
}
