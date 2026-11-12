package ir.altek.link;
import android.content.Context;
import android.content.Intent;
import android.content.UriPermission;
import android.net.Uri;

/** Called only on Android 4.4 and newer. */
final class PersistedAccess {
  static void takeRead(Context c, Uri uri) {
    c.getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
  }
  static void takeReadWrite(Context c, Uri uri) {
    c.getContentResolver().takePersistableUriPermission(uri,
        Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
  }
  static boolean hasRead(Context c, Uri uri) {
    for (UriPermission p : c.getContentResolver().getPersistedUriPermissions())
      if (p.isReadPermission() && uri.equals(p.getUri())) return true;
    return false;
  }
}
