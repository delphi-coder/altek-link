package ir.altek.link;

import android.content.*;
import android.os.Build;

public final class BootReceiver extends BroadcastReceiver {
  public void onReceive(Context c, Intent i) {
    if (!c.getSharedPreferences("settings", 0).getBoolean("enabled", false)) return;
    Intent s = new Intent(c, LinkService.class);
    try {
      if (Build.VERSION.SDK_INT >= 26)
        Context.class.getMethod("startForegroundService", Intent.class).invoke(c, s);
      else c.startService(s);
    } catch (Exception e) {
      android.util.Log.e("ALTEK", "Boot service start", e);
    }
  }
}
