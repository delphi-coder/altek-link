package ir.altek.link;

import android.app.Application;
import java.io.*;

public final class LinkApp extends Application {
  public void onCreate() {
    super.onCreate();
    DebugLog.init(this, "starting");
    final Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
    final File saved = new File(getFilesDir(), "last-crash.txt");
    if (saved.exists()) {
      try {
        String old = new String(Identity.bytes(saved), "UTF-8");
        DebugLog.event("PREVIOUS_CRASH", old);
        saved.delete();
      } catch (Exception ignored) {
      }
    }
    Thread.setDefaultUncaughtExceptionHandler(
        new Thread.UncaughtExceptionHandler() {
          public void uncaughtException(Thread t, Throwable e) {
            try {
              StringBuilder text = new StringBuilder(e.getClass().getName());
              for (StackTraceElement x : e.getStackTrace())
                if (x.getClassName().startsWith("ir.altek"))
                  text.append(" @ ").append(x.toString());
              FileOutputStream out = new FileOutputStream(saved);
              out.write(text.toString().getBytes("UTF-8"));
              out.close();
              DebugLog.error("CRASH", e);
            } catch (Exception ignored) {
            }
            if (previous != null) previous.uncaughtException(t, e);
            else android.os.Process.killProcess(android.os.Process.myPid());
          }
        });
  }
}
