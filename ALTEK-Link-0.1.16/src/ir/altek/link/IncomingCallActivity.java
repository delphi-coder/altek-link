package ir.altek.link;

import android.app.Activity;
import android.content.*;
import android.os.*;
import android.view.*;
import android.widget.*;

/** Full-screen incoming call screen (WhatsApp/Telegram style). Shown even when the app is in
 *  the background or the phone is locked: wakes the display and dismisses the keyguard.
 *  Accept/Decline are connected to the existing LinkService answer/hangup logic. */
public final class IncomingCallActivity extends Activity {
  private LinkService.Call attached;
  private boolean answered;
  private final Handler handler = new Handler();
  private PowerManager.WakeLock wake;
  private TextView nameText, kindText, hintText;
  private ImageButton acceptBtn, declineBtn;
  private AvatarView avatar;

  public void onCreate(Bundle b) {
    super.onCreate(b);
    LinkService s = LinkService.current;
    if (s == null || s.call == null || s.call.outgoing || s.call.accepted) {
      finish();
      return;
    }
    attached = s.call;

    // Wake the screen; on API 27+ use the modern API via reflection, otherwise the window flags.
    if (Build.VERSION.SDK_INT >= 27) {
      try {
        Activity.class.getMethod("setShowWhenLocked", boolean.class).invoke(this, Boolean.TRUE);
        Activity.class.getMethod("setTurnScreenOn", boolean.class).invoke(this, Boolean.TRUE);
        Object km = getSystemService(KEYGUARD_SERVICE);
        Class<?> kmc = Class.forName("android.app.KeyguardManager");
        kmc.getMethod(
                "requestDismissKeyguard",
                Activity.class,
                Class.forName("android.app.KeyguardManager$KeyguardDismissCallback"))
            .invoke(km, this, null);
      } catch (Exception e) {
        DebugLog.error("LOCKSCREEN", e);
      }
    }
    getWindow()
        .addFlags(
            WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED
                | WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON
                | WindowManager.LayoutParams.FLAG_DISMISS_KEYGUARD
                | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    try {
      PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
      wake =
          pm.newWakeLock(
              PowerManager.ACQUIRE_CAUSES_WAKEUP | PowerManager.SCREEN_BRIGHT_WAKE_LOCK,
              "altek:incoming");
      wake.acquire(15000);
    } catch (Exception e) {
      DebugLog.error("WAKE", e);
    }

    // UI
    FrameLayout root = new FrameLayout(this);
    root.setBackgroundColor(0xff0b141a);
    LinearLayout col = new LinearLayout(this);
    col.setOrientation(1);
    col.setGravity(android.view.Gravity.CENTER_HORIZONTAL);
    col.setPadding(dp(24), dp(48), dp(24), dp(48));

    avatar = new AvatarView(this);
    Store.Peer p = s.db == null ? null : s.db.peer(attached.peer);
    avatar.bind(attached.peer, p == null ? "Contact" : p.name);
    col.addView(avatar, new LinearLayout.LayoutParams(dp(120), dp(120)));

    nameText = new TextView(this);
    nameText.setText(p == null ? "Unknown contact" : p.name);
    nameText.setTextSize(26);
    nameText.setTextColor(0xffffffff);
    nameText.setGravity(android.view.Gravity.CENTER);
    nameText.setPadding(0, dp(16), 0, 0);
    col.addView(nameText, new LinearLayout.LayoutParams(-1, -2));

    kindText = new TextView(this);
    kindText.setText(
        "screen".equals(attached.kind)
            ? "Remote control request"
            : "Incoming " + attached.kind + " call");
    kindText.setTextSize(15);
    kindText.setTextColor(0xff8f9bb0);
    kindText.setGravity(android.view.Gravity.CENTER);
    kindText.setPadding(0, dp(6), 0, 0);
    col.addView(kindText, new LinearLayout.LayoutParams(-1, -2));

    hintText = new TextView(this);
    hintText.setText("Swipe right to answer  •  Swipe left to decline");
    hintText.setTextSize(12);
    hintText.setTextColor(0xff5b6b7c);
    hintText.setGravity(android.view.Gravity.CENTER);
    hintText.setPadding(0, dp(24), 0, 0);
    col.addView(hintText, new LinearLayout.LayoutParams(-1, -2));

    LinearLayout row = new LinearLayout(this);
    row.setOrientation(0);
    row.setGravity(android.view.Gravity.CENTER);
    row.setPadding(0, dp(40), 0, 0);

    declineBtn = new ImageButton(this);
    declineBtn.setImageDrawable(new Icons("end", 0xffffffff));
    declineBtn.setBackgroundColor(0xffe53935);
    declineBtn.setPadding(dp(20), dp(20), dp(20), dp(20));
    declineBtn.setContentDescription("Decline");
    declineBtn.setOnClickListener(
        new View.OnClickListener() {
          public void onClick(View v) {
            decline();
          }
        });
    row.addView(declineBtn, new LinearLayout.LayoutParams(dp(72), dp(72)));

    acceptBtn = new ImageButton(this);
    acceptBtn.setImageDrawable(
        new Icons("audio".equals(attached.kind) ? "audio" : "video", 0xffffffff));
    acceptBtn.setBackgroundColor(0xff43a047);
    acceptBtn.setPadding(dp(20), dp(20), dp(20), dp(20));
    acceptBtn.setContentDescription("Accept");
    acceptBtn.setOnClickListener(
        new View.OnClickListener() {
          public void onClick(View v) {
            accept();
          }
        });
    LinearLayout.LayoutParams ap = new LinearLayout.LayoutParams(dp(72), dp(72));
    ap.leftMargin = dp(48);
    row.addView(acceptBtn, ap);
    col.addView(row, new LinearLayout.LayoutParams(-1, -2));

    root.addView(col, new FrameLayout.LayoutParams(-1, -1));
    setContentView(root);

    // Swipe handling on the root view.
    root.setOnTouchListener(
        new View.OnTouchListener() {
          private float downX, downY;

          public boolean onTouch(View v, MotionEvent e) {
            switch (e.getAction()) {
              case MotionEvent.ACTION_DOWN:
                downX = e.getX();
                downY = e.getY();
                return true;
              case MotionEvent.ACTION_UP:
                float dx = e.getX() - downX, dy = e.getY() - downY;
                if (Math.abs(dx) > dp(80) && Math.abs(dx) > Math.abs(dy) * 2) {
                  if (dx > 0) accept();
                  else decline();
                  return true;
                }
                return false;
            }
            return false;
          }
        });

    // If the call ends/expires elsewhere, close this screen.
    handler.post(
        new Runnable() {
          public void run() {
            LinkService s = LinkService.current;
            if (s == null || s.call != attached) {
              finish();
              return;
            }
            handler.postDelayed(this, 500);
          }
        });
  }

  private void accept() {
    if (answered || attached == null) return;
    answered = true;
    final LinkService.Call c = attached;
    mediaPermissions(
        c.kind,
        new Runnable() {
          public void run() {
            LinkService s = LinkService.current;
            if (s != null && s.call == c) {
              startActivity(
                  new Intent(
                      IncomingCallActivity.this,
                      "screen".equals(c.kind) ? RemoteControlActivity.class : MediaActivity.class));
            }
            finish();
          }
        });
  }

  private void decline() {
    LinkService s = LinkService.current;
    if (s != null && s.call == attached) s.hangup();
    finish();
  }

  private void mediaPermissions(String kind, Runnable done) {
    if ("view".equals(kind) || "screen".equals(kind)) {
      done.run();
      return;
    }
    permissions(
        "video".equals(kind)
            ? new String[] {"android.permission.CAMERA", "android.permission.RECORD_AUDIO"}
            : new String[] {"android.permission.RECORD_AUDIO"},
        done);
  }

  private void permissions(final String[] wanted, final Runnable done) {
    try {
      if (Build.VERSION.SDK_INT < 23) {
        done.run();
        return;
      }
      java.util.ArrayList<String> missing = new java.util.ArrayList<String>();
      for (String p : wanted)
        if (((Integer)
                Activity.class.getMethod("checkSelfPermission", String.class).invoke(this, p))
            != android.content.pm.PackageManager.PERMISSION_GRANTED) missing.add(p);
      if (missing.isEmpty()) {
        done.run();
        return;
      }
      Activity.class
          .getMethod("requestPermissions", String[].class, int.class)
          .invoke(
              this,
              new Object[] {missing.toArray(new String[missing.size()]), Integer.valueOf(44)});
      afterPermission = done;
    } catch (Exception e) {
      afterPermission = null;
      DebugLog.error("PERMISSION", e);
      done.run();
    }
  }

  private Runnable afterPermission;

  public void onRequestPermissionsResult(int request, String[] p, int[] results) {
    if (request == 44 && afterPermission != null) {
      Runnable r = afterPermission;
      afterPermission = null;
      r.run();
    } else {
      super.onRequestPermissionsResult(request, p, results);
    }
  }

  protected void onDestroy() {
    super.onDestroy();
    handler.removeCallbacksAndMessages(null);
    if (wake != null && wake.isHeld()) {
      try {
        wake.release();
      } catch (Exception ignored) {
      }
    }
  }

  private int dp(int v) {
    return (int) (v * getResources().getDisplayMetrics().density + 0.5f);
  }
}
