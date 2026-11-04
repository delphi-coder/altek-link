package ir.altek.link;

import java.io.*;
import java.security.MessageDigest;
import java.util.UUID;

/** Bounded binary framing. No Java serialization on the wire. */
public final class Wire {
  public static final int VERSION = 1, PORT = 45871, DISCOVERY = 45872, LOG = 45879;
  public static final int MAX_JSON = 65536, MAX_MEDIA = 2 * 1024 * 1024;

  public static String uid() {
    return UUID.randomUUID().toString();
  }

  public static byte[] read(DataInputStream in, int max) throws IOException {
    int n = in.readInt();
    if (n < 0 || n > max) throw new IOException("Invalid frame length " + n);
    byte[] b = new byte[n];
    in.readFully(b);
    return b;
  }

  public static void write(DataOutputStream out, byte[] b) throws IOException {
    out.writeInt(b.length);
    out.write(b);
    out.flush();
  }

  public static String readText(DataInputStream in) throws IOException {
    return new String(read(in, MAX_JSON), "UTF-8");
  }

  public static void text(DataOutputStream out, String s) throws IOException {
    byte[] b = s.getBytes("UTF-8");
    if (b.length > MAX_JSON) throw new IOException("Message too large");
    write(out, b);
  }

  public static String hex(byte[] b) {
    StringBuilder s = new StringBuilder();
    for (byte v : b) s.append(String.format(java.util.Locale.US, "%02x", v & 255));
    return s.toString();
  }

  public static String digest(byte[] b) throws Exception {
    return hex(MessageDigest.getInstance("SHA-256").digest(b));
  }

  public static String safeName(String s) {
    s = s.replace('\\', '_').replace('/', '_').replaceAll("[\\p{Cntrl}]", "_");
    return s.length() > 120 ? s.substring(0, 120) : s;
  }

  public static void close(Closeable c) {
    if (c != null)
      try {
        c.close();
      } catch (Exception ignored) {
      }
  }

  // Android 4.1 sockets do not implement java.io.Closeable.
  public static void close(java.net.Socket socket) {
    if (socket != null)
      try {
        socket.close();
      } catch (Exception ignored) {
      }
  }

  public static void close(java.net.ServerSocket socket) {
    if (socket != null)
      try {
        socket.close();
      } catch (Exception ignored) {
      }
  }
}
