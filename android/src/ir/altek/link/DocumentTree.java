package ir.altek.link;

import android.content.Context;
import android.net.Uri;
import android.provider.DocumentsContract;

/** Loaded only on Android 5.0 and newer. */
final class DocumentTree {
  private DocumentTree() {}
  static Uri create(Context c, Uri tree, String name) throws Exception {
    Uri parent = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
    return DocumentsContract.createDocument(c.getContentResolver(), parent, "application/octet-stream", name);
  }
  static boolean hasName(Context c, Uri uri, String name) throws Exception {
    Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(uri, DocumentsContract.getTreeDocumentId(uri));
    android.database.Cursor cursor = c.getContentResolver().query(children,
        new String[] {DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null, null);
    if (cursor == null) throw new java.io.IOException("Cannot list receive folder");
    try { while (cursor.moveToNext()) if (name.equals(cursor.getString(0))) return true; }
    finally { cursor.close(); }
    return false;
  }
  static Uri rename(Context c, Uri uri, String name) throws Exception {
    return DocumentsContract.renameDocument(c.getContentResolver(), uri, name);
  }
  static void delete(Context c, Uri uri) throws Exception {
    DocumentsContract.deleteDocument(c.getContentResolver(), uri);
  }
}
