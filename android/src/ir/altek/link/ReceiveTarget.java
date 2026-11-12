package ir.altek.link;

import android.content.Context;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import java.io.*;
import java.nio.channels.FileChannel;
import java.security.MessageDigest;

/** A resumable private file or document in the user-selected tree. */
final class ReceiveTarget {
  private final Context context;
  private final String id, filename;
  private File part, result;
  private Uri document;
  private final SharedPreferences state;
  ReceiveTarget(Context c, String id, String name) throws Exception {
    context = c; this.id = id; filename = Wire.safeName(name);
    state = c.getSharedPreferences("receive_parts", 0);
    String old = state.getString(id, "");
    String tree = c.getSharedPreferences("settings", 0).getString("receive_tree", "");
    if (old.startsWith("file:")) {
      part = new File(old.substring(5));
      File folder = part.getParentFile();
      boolean appFolder = folder.equals(new File(c.getFilesDir(), "received"));
      result = new File(folder, appFolder ? id : id + "_" + filename);
    } else if (old.length() > 0 || (android.os.Build.VERSION.SDK_INT >= 21 && tree.length() > 0)) {
      if (old.length() > 0) document = Uri.parse(old);
      else {
        Uri treeUri = Uri.parse(tree);
        document = DocumentTree.create(c, treeUri, id + "_" + filename + ".part");
        if (document == null) throw new IOException("Cannot create file in selected folder");
        if (!state.edit().putString(id, document.toString()).commit()) throw new IOException("Cannot save download state");
      }
    } else {
      String legacy = c.getSharedPreferences("settings", 0).getString("receive_path", "");
      File folder = legacy.length() > 0 ? new File(legacy) : new File(c.getFilesDir(), "received");
      if (!folder.exists() && !folder.mkdirs()) throw new IOException("Cannot create receive folder");
      part = new File(folder, id + ".part");
      result = new File(folder, legacy.length() > 0 ? id + "_" + filename : id);
      if (!state.edit().putString(id, "file:" + part.getAbsolutePath()).commit()) throw new IOException("Cannot save download state");
    }
  }
  long size() throws IOException {
    if (document == null) return part.exists() ? part.length() : 0;
    ParcelFileDescriptor p = context.getContentResolver().openFileDescriptor(document, "r");
    if (p == null) throw new IOException("Cannot open partial file");
    try { long n = p.getStatSize(); if (n < 0) throw new IOException("Folder does not support resuming"); return n; }
    finally { p.close(); }
  }
  void checkSpace(long needed) throws IOException {
    if (document != null || part == null) return;
    android.os.StatFs stat = new android.os.StatFs(part.getParent());
    long available = (long) stat.getAvailableBlocks() * (long) stat.getBlockSize();
    if (needed > available) throw new IOException("Not enough space in receive folder");
  }
  InputStream input() throws IOException {
    InputStream in = document == null ? new FileInputStream(part) : context.getContentResolver().openInputStream(document);
    if (in == null) throw new IOException("Cannot read partial file");
    return in;
  }
  OutputStream output(long offset) throws IOException {
    if (document == null) {
      RandomAccessFile f = new RandomAccessFile(part, "rw");
      f.setLength(offset); f.seek(offset);
      return new RandomOutput(f);
    }
    ParcelFileDescriptor p = context.getContentResolver().openFileDescriptor(document, "rw");
    if (p == null) throw new IOException("Cannot write partial file");
    try {
      FileOutputStream out = new FileOutputStream(p.getFileDescriptor());
      FileChannel channel = out.getChannel();
      channel.truncate(offset); channel.position(offset);
      return new DocumentOutput(out, p);
    } catch (IOException e) { p.close(); throw e; }
  }
  void digestPrefix(MessageDigest hash, long count) throws IOException {
    if (count == 0) return;
    InputStream in = input(); byte[] b = new byte[262144];
    try { while (count > 0) { int n = in.read(b, 0, (int)Math.min(b.length, count)); if (n < 0) throw new EOFException(); hash.update(b, 0, n); count -= n; } }
    finally { in.close(); }
  }
  private String candidate(int index) {
    if (index == 0) return filename;
    int dot = filename.lastIndexOf('.');
    if (dot <= 0) dot = filename.length();
    return filename.substring(0, dot) + " (" + index + ")" + filename.substring(dot);
  }
  String finish() throws Exception {
    synchronized (ReceiveTarget.class) {
      for (int index = 0; index < 100000; index++) {
        String name = candidate(index);
        if (document == null) {
          File target = new File(part.getParentFile(), name);
          if (!target.createNewFile()) continue;
          if (!part.renameTo(target)) {
            target.delete();
            throw new IOException("Rename failed");
          }
          state.edit().remove(id).apply();
          return target.getAbsolutePath();
        }
        if (DocumentTree.hasName(context, document, name)) continue;
        Uri renamed = DocumentTree.rename(context, document, name);
        if (renamed == null) throw new IOException("Cannot finish file in selected folder");
        state.edit().remove(id).apply();
        return renamed.toString();
      }
      throw new IOException("Too many files with the same name");
    }
  }
  void discard() {
    try { if (document != null) DocumentTree.delete(context, document); else if (part != null) part.delete(); }
    catch (Exception ignored) {}
    state.edit().remove(id).apply();
  }
  private static final class RandomOutput extends OutputStream {
    private final RandomAccessFile f; RandomOutput(RandomAccessFile f) { this.f = f; }
    public void write(int b) throws IOException { f.write(b); }
    public void write(byte[] b, int off, int n) throws IOException { f.write(b, off, n); }
    public void close() throws IOException { f.close(); }
  }
  private static final class DocumentOutput extends OutputStream {
    private final FileOutputStream out; private final ParcelFileDescriptor p;
    DocumentOutput(FileOutputStream out, ParcelFileDescriptor p) { this.out = out; this.p = p; }
    public void write(int b) throws IOException { out.write(b); }
    public void write(byte[] b, int off, int n) throws IOException { out.write(b, off, n); }
    public void close() throws IOException { try { out.close(); } finally { p.close(); } }
  }
}
