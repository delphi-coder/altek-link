package ir.altek.link;

import android.content.Context;
import android.media.*;
import android.net.Uri;
import android.os.*;

/** Incoming audio/video ringing, owned by the pending call rather than by an Activity. */
public final class IncomingRinger {
  private final Context context;
  private final Handler main = new Handler(Looper.getMainLooper());
  private volatile long generation;
  private Ringtone tone;
  private Vibrator vibrator;
  private Runnable tick;
  private boolean vibrating;

  IncomingRinger(Context c) {
    context = c;
  }

  public synchronized void start() {
    final long ticket = ++generation;
    main.post(
        new Runnable() {
          public void run() {
            if (ticket != generation) return;
            stopNow();
            final long deadline = SystemClock.elapsedRealtime() + 45000;
            try {
              Uri uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE);
              tone = RingtoneManager.getRingtone(context, uri);
              vibrator = (Vibrator) context.getSystemService(Context.VIBRATOR_SERVICE);
            } catch (Exception e) {
              DebugLog.error("RING_START", e);
            }
            tick =
                new Runnable() {
                  public void run() {
                    if (ticket != generation || SystemClock.elapsedRealtime() >= deadline) {
                      stopNow();
                      return;
                    }
                    try {
                      AudioManager audio =
                          (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
                      int mode = audio.getRingerMode();
                      if (mode == AudioManager.RINGER_MODE_NORMAL
                          && audio.getStreamVolume(AudioManager.STREAM_RING) > 0) {
                        if (tone != null && !tone.isPlaying()) tone.play();
                      } else if (tone != null && tone.isPlaying()) tone.stop();
                      if (mode == AudioManager.RINGER_MODE_VIBRATE
                          && !vibrating
                          && vibrator != null) {
                        vibrator.vibrate(new long[] {0, 500, 500}, 0);
                        vibrating = true;
                      } else if (mode != AudioManager.RINGER_MODE_VIBRATE && vibrating) {
                        vibrator.cancel();
                        vibrating = false;
                      }
                    } catch (Exception e) {
                      DebugLog.error("RING", e);
                    }
                    main.postDelayed(this, 700);
                  }
                };
            tick.run();
            DebugLog.event("RING", "started");
          }
        });
  }

  public synchronized void stop() {
    final long ticket = ++generation;
    main.post(
        new Runnable() {
          public void run() {
            if (ticket == generation) stopNow();
          }
        });
  }

  private void stopNow() {
    if (tick != null) {
      main.removeCallbacks(tick);
      tick = null;
    }
    if (tone != null) {
      try {
        tone.stop();
      } catch (Exception ignored) {
      }
      tone = null;
    }
    if (vibrator != null && vibrating) vibrator.cancel();
    vibrating = false;
  }
}
