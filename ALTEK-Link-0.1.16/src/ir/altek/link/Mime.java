package ir.altek.link;

import java.io.*;
import java.util.Locale;

/**
 * Resolves a specific MIME type for an attachment. Peers often send a generic type such as
 * application/octet-stream (picker returned null), which makes Android offer only apps that accept
 * every type (text editors, Termux). We fall back to the file extension and then to magic bytes.
 */
public final class Mime {
  public static final String GENERIC = "application/octet-stream";

  private Mime() {}

  public static boolean isGeneric(String m) {
    if (m == null) return true;
    m = m.trim().toLowerCase(Locale.US);
    return m.length() == 0
        || m.indexOf('/') < 0
        || m.equals(GENERIC)
        || m.equals("binary/octet-stream")
        || m.equals("application/unknown")
        || m.equals("application/force-download")
        || m.equals("application/download")
        || m.endsWith("/*");
  }

  public static String resolve(String stored, String filename, File file) {
    if (!isGeneric(stored)) {
      String m = stored.trim().toLowerCase(Locale.US);
      int semi = m.indexOf(';');
      return semi > 0 ? m.substring(0, semi).trim() : m;
    }
    String byName = fromName(filename);
    if (byName != null) return byName;
    String bySniff = sniff(file);
    return bySniff != null ? bySniff : GENERIC;
  }

  static String fromName(String name) {
    if (name == null) return null;
    int dot = name.lastIndexOf('.');
    if (dot < 0 || dot == name.length() - 1) return null;
    String ext = name.substring(dot + 1).toLowerCase(Locale.US);
    String m = builtin(ext);
    if (m != null) return m;
    try {
      m = android.webkit.MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext);
    } catch (Throwable ignored) {
      // Not running on Android (JVM unit tests).
    }
    return isGeneric(m) ? null : m;
  }

  private static String builtin(String ext) {
    if (ext.equals("jpg") || ext.equals("jpeg") || ext.equals("jpe") || ext.equals("jfif"))
      return "image/jpeg";
    if (ext.equals("png")) return "image/png";
    if (ext.equals("gif")) return "image/gif";
    if (ext.equals("webp")) return "image/webp";
    if (ext.equals("bmp")) return "image/bmp";
    if (ext.equals("heic")) return "image/heic";
    if (ext.equals("heif")) return "image/heif";
    if (ext.equals("svg")) return "image/svg+xml";
    if (ext.equals("mp4") || ext.equals("m4v")) return "video/mp4";
    if (ext.equals("3gp") || ext.equals("3gpp")) return "video/3gpp";
    if (ext.equals("webm")) return "video/webm";
    if (ext.equals("mkv")) return "video/x-matroska";
    if (ext.equals("mov")) return "video/quicktime";
    if (ext.equals("mp3")) return "audio/mpeg";
    if (ext.equals("m4a")) return "audio/mp4";
    if (ext.equals("aac")) return "audio/aac";
    if (ext.equals("ogg") || ext.equals("oga") || ext.equals("opus")) return "audio/ogg";
    if (ext.equals("wav")) return "audio/x-wav";
    if (ext.equals("pdf")) return "application/pdf";
    if (ext.equals("zip")) return "application/zip";
    if (ext.equals("apk")) return "application/vnd.android.package-archive";
    if (ext.equals("txt") || ext.equals("log")) return "text/plain";
    if (ext.equals("doc")) return "application/msword";
    if (ext.equals("docx"))
      return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
    if (ext.equals("xls")) return "application/vnd.ms-excel";
    if (ext.equals("xlsx"))
      return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
    if (ext.equals("ppt")) return "application/vnd.ms-powerpoint";
    if (ext.equals("pptx"))
      return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
    return null;
  }

  /** Identify a few common formats from their first bytes. */
  static String sniff(File f) {
    if (f == null || !f.isFile()) return null;
    byte[] b = new byte[16];
    int n = 0;
    FileInputStream in = null;
    try {
      in = new FileInputStream(f);
      n = in.read(b);
    } catch (IOException e) {
      return null;
    } finally {
      Wire.close(in);
    }
    if (n < 4) return null;
    if ((b[0] & 255) == 0xFF && (b[1] & 255) == 0xD8 && (b[2] & 255) == 0xFF) return "image/jpeg";
    if ((b[0] & 255) == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') return "image/png";
    if (b[0] == 'G' && b[1] == 'I' && b[2] == 'F' && b[3] == '8') return "image/gif";
    if (n >= 12
        && b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F'
        && b[8] == 'W' && b[9] == 'E' && b[10] == 'B' && b[11] == 'P') return "image/webp";
    if (b[0] == '%' && b[1] == 'P' && b[2] == 'D' && b[3] == 'F') return "application/pdf";
    if (n >= 12 && b[4] == 'f' && b[5] == 't' && b[6] == 'y' && b[7] == 'p') {
      if (b[8] == 'h' && b[9] == 'e' && b[10] == 'i') return "image/heic";
      return "video/mp4";
    }
    return null;
  }
}
