package ir.altek.link;

import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import java.io.*;
import java.util.*;

/** Android 5+ document-tree operations; document IDs stay inside the selected tree. */
final class WebDocuments {
  static final class Entry {
    String id, name; long size, modified; boolean folder;
    Entry(String i,String n,long s,boolean f,long m) { id=i; name=n; size=s; folder=f; modified=m; }
  }
  static String root(Uri tree) { return DocumentsContract.getTreeDocumentId(tree); }
  static Uri uri(Uri tree,String id) { return DocumentsContract.buildDocumentUriUsingTree(tree,id); }
  static List<Entry> list(Context c,Uri tree,String id) throws IOException {
    ArrayList<Entry> result=new ArrayList<Entry>();
    Uri children=DocumentsContract.buildChildDocumentsUriUsingTree(tree,id);
    String[] cols={DocumentsContract.Document.COLUMN_DOCUMENT_ID,DocumentsContract.Document.COLUMN_DISPLAY_NAME,DocumentsContract.Document.COLUMN_MIME_TYPE,DocumentsContract.Document.COLUMN_SIZE,DocumentsContract.Document.COLUMN_LAST_MODIFIED};
    Cursor cur=c.getContentResolver().query(children,cols,null,null,null);
    if(cur==null) throw new IOException("Cannot read folder");
    try { while(cur.moveToNext()) result.add(new Entry(cur.getString(0),cur.getString(1),cur.isNull(3)?-1:cur.getLong(3),DocumentsContract.Document.MIME_TYPE_DIR.equals(cur.getString(2)),cur.isNull(4)?0:cur.getLong(4))); }
    finally { cur.close(); }
    return result;
  }
  static Uri create(Context c,Uri tree,String parent,String name,String mime) throws Exception {
    return DocumentsContract.createDocument(c.getContentResolver(),uri(tree,parent),mime,name);
  }
  static void delete(Context c,Uri doc) { try { DocumentsContract.deleteDocument(c.getContentResolver(),doc); } catch(Exception ignored) {} }
}
