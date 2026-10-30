package ir.altek.link;

import android.content.Context;
import android.os.Build;
import android.util.Log;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.atomic.AtomicLong;

/** Debug-only UDP diagnostics; never log message bodies, file contents, keys, or tokens. */
public final class DebugLog {
  private static final BlockingQueue<String> queue = new ArrayBlockingQueue<String>(200);
  private static final AtomicLong sequence = new AtomicLong();
  private static volatile boolean started;
  private static volatile String id = "starting";

  public static synchronized void init(Context context, String identity) {
    id = identity.substring(0, 8);
    if (started) return;
    started = true;
    Thread thread =
        new Thread(
            new Runnable() {
              public void run() {
                DatagramSocket socket = null;
                try {
                  InetAddress destination = InetAddress.getByName(DebugUdpConfig.HOST);
                  socket = new DatagramSocket();
                  while (true) {
                    byte[] bytes = queue.take().getBytes("UTF-8");
                    socket.send(new DatagramPacket(bytes, bytes.length, destination, Wire.LOG));
                  }
                } catch (Exception ignored) {
                } finally {
                  if (socket != null) socket.close();
                }
              }
            },
            "udp-debug");
    thread.setDaemon(true);
    thread.start();
  }

  public static void event(String component, String message) {
    String line =
        System.currentTimeMillis()
            + " #"
            + sequence.incrementAndGet()
            + " ["
            + id
            + " "
            + Build.MODEL
            + "] "
            + component
            + " "
            + message.replace('\n', ' ');
    if (line.length() > 900) line = line.substring(0, 900);
    Log.i("ALTEK", line);
    queue.offer(line);
  }

  public static void error(String component, Throwable error) {
    StringBuilder summary = new StringBuilder();
    Throwable current = error;
    int depth = 0;
    Log.e("ALTEK", component, error);
    while (current != null && depth < 4) {
      if (depth > 0) summary.append(" <- ");
      summary.append(current.getClass().getSimpleName());
      for (StackTraceElement frame : current.getStackTrace()) {
        if (frame.getClassName().startsWith("ir.altek")) {
          summary.append(" @ ").append(frame.toString());
          if (summary.length() > 800) break;
        }
      }
      if (summary.length() > 800) break;
      current = current.getCause() == current ? null : current.getCause();
      depth++;
    }
    event(component, summary.toString());
  }
}
