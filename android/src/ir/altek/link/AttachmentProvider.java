package ir.altek.link;

import android.content.*;
import android.database.*;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import java.io.*;
import org.json.*;

/** Read-only, private files exposed to a selected viewer using an Android URI permission grant. */
public final class AttachmentProvider extends ContentProvider {
  public boolean onCreate() {
    return true;
  }

  private Store.Msg message(Uri uri) throws FileNotFoundException {
    String id = uri.getLastPathSegment();
    if (id == null || !id.matches("[0-9a-fA-F-]{36}")) throw new FileNotFoundException();
    Store store = new Store(getContext());
    try {
      Store.Msg m = store.message(id);
      if (m == null || m.path == null || m.path.isEmpty()) throw new FileNotFoundException();
      File f = new File(m.path);
      if (!f.getCanonicalPath()
              .startsWith(getContext().getFilesDir().getCanonicalPath() + File.separator)
          || !f.isFile()) throw new FileNotFoundException();
      return m;
    } catch (IOException e) {
      throw new FileNotFoundException();
    } finally {
      store.close();
    }
  }

  public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
    if (!"r".equals(mode)) throw new FileNotFoundException();
    return ParcelFileDescriptor.open(
        new File(message(uri).path), ParcelFileDescriptor.MODE_READ_ONLY);
  }

  public String getType(Uri uri) {
    try {
      Store.Msg m = message(uri);
      JSONObject j = new JSONObject(m.body);
      return Mime.resolve(
          j.optString("mime", Mime.GENERIC), j.optString("filename", null), new File(m.path));
    } catch (Exception e) {
      return Mime.GENERIC;
    }
  }

  public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sort) {
    try {
      Store.Msg m = message(uri);
      JSONObject j = new JSONObject(m.body);
      String[] columns = projection == null ? new String[] {"_display_name", "_size"} : projection;
      MatrixCursor c = new MatrixCursor(columns);
      Object[] values = new Object[columns.length];
      for (int i = 0; i < columns.length; i++)
        values[i] =
            "_display_name".equals(columns[i])
                ? j.getString("filename")
                : ("_size".equals(columns[i]) ? Long.valueOf(new File(m.path).length()) : null);
      c.addRow(values);
      return c;
    } catch (Exception e) {
      return null;
    }
  }

  public Uri insert(Uri u, ContentValues v) {
    throw new UnsupportedOperationException();
  }

  public int delete(Uri u, String s, String[] a) {
    throw new UnsupportedOperationException();
  }

  public int update(Uri u, ContentValues v, String s, String[] a) {
    throw new UnsupportedOperationException();
  }
}
