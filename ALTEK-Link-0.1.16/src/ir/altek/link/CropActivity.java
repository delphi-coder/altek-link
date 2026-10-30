package ir.altek.link;

import android.app.Activity;
import android.content.*;
import android.graphics.*;
import android.media.ExifInterface;
import android.os.Bundle;
import android.view.*;
import android.widget.*;
import java.io.*;

/** In-app square crop with circular preview, pan/pinch, bounded decoding and 256px output. */
public final class CropActivity extends Activity {
  private CropView crop;
  private TextView info;
  private Button save;
  private volatile boolean closed;

  protected void onCreate(Bundle state) {
    super.onCreate(state);
    LinearLayout root = new LinearLayout(this);
    root.setOrientation(1);
    root.setBackgroundColor(0xff172a2b);
    info = new TextView(this);
    info.setTextColor(Color.WHITE);
    info.setPadding(16, 16, 16, 16);
    info.setText("Loading photo...");
    root.addView(info);
    crop = new CropView(this);
    root.addView(crop, new LinearLayout.LayoutParams(-1, 0, 1));
    LinearLayout row = new LinearLayout(this);
    root.addView(row);
    Button cancel = new Button(this);
    cancel.setText("Cancel");
    row.addView(cancel, new LinearLayout.LayoutParams(0, -2, 1));
    cancel.setOnClickListener(
        new View.OnClickListener() {
          public void onClick(View v) {
            finish();
          }
        });
    Button rotate = new Button(this);
    rotate.setText("Rotate");
    row.addView(rotate, new LinearLayout.LayoutParams(0, -2, 1));
    rotate.setOnClickListener(
        new View.OnClickListener() {
          public void onClick(View v) {
            crop.rotate();
          }
        });
    save = new Button(this);
    save.setText("Save");
    save.setEnabled(false);
    row.addView(save, new LinearLayout.LayoutParams(0, -2, 1));
    save.setOnClickListener(
        new View.OnClickListener() {
          public void onClick(View v) {
            final Bitmap result = crop.result();
            if (result == null) return;
            save.setEnabled(false);
            info.setText("Saving...");
            new Thread(
                    new Runnable() {
                      public void run() {
                        try {
                          Profile.save(CropActivity.this, result);
                          runOnUiThread(
                              new Runnable() {
                                public void run() {
                                  setResult(RESULT_OK);
                                  finish();
                                }
                              });
                        } catch (Exception e) {
                          failed(e);
                        } finally {
                          result.recycle();
                        }
                      }
                    },
                    "profile-save")
                .start();
          }
        });
    setContentView(root);
    new Thread(
            new Runnable() {
              public void run() {
                load();
              }
            },
            "profile-decode")
        .start();
  }

  private void load() {
    File temp = null;
    try {
      temp = File.createTempFile("profile-source", ".img", getCacheDir());
      InputStream in = getContentResolver().openInputStream(getIntent().getData());
      if (in == null) throw new IOException("No image");
      try {
        FileOutputStream out = new FileOutputStream(temp);
        try {
          byte[] b = new byte[8192];
          int n, total = 0;
          while ((n = in.read(b)) != -1) {
            total += n;
            if (total > 25 * 1024 * 1024) throw new IOException("Image exceeds 25 MB");
            out.write(b, 0, n);
          }
        } finally {
          out.close();
        }
      } finally {
        in.close();
      }
      BitmapFactory.Options o = new BitmapFactory.Options();
      o.inJustDecodeBounds = true;
      BitmapFactory.decodeFile(temp.getPath(), o);
      if (o.outWidth <= 0 || o.outHeight <= 0) throw new IOException("Unsupported image");
      o.inSampleSize = 1;
      while (o.outWidth / o.inSampleSize > 1024 || o.outHeight / o.inSampleSize > 1024)
        o.inSampleSize *= 2;
      o.inJustDecodeBounds = false;
      Bitmap source = BitmapFactory.decodeFile(temp.getPath(), o);
      if (source == null) throw new IOException("Decode failed");
      try {
        int orientation =
            new ExifInterface(temp.getPath()).getAttributeInt(ExifInterface.TAG_ORIENTATION, 1);
        Matrix m = new Matrix();
        switch (orientation) {
          case 2:
            m.setScale(-1, 1);
            break;
          case 3:
            m.setRotate(180);
            break;
          case 4:
            m.setScale(1, -1);
            break;
          case 5:
            m.setRotate(90);
            m.postScale(-1, 1);
            break;
          case 6:
            m.setRotate(90);
            break;
          case 7:
            m.setRotate(90);
            m.postScale(1, -1);
            break;
          case 8:
            m.setRotate(270);
            break;
        }
        if (!m.isIdentity()) {
          Bitmap oriented =
              Bitmap.createBitmap(source, 0, 0, source.getWidth(), source.getHeight(), m, true);
          if (oriented != source) source.recycle();
          source = oriented;
        }
      } catch (IOException ignored) {
      }
      final Bitmap b = source;
      runOnUiThread(
          new Runnable() {
            public void run() {
              if (closed) {
                b.recycle();
                return;
              }
              crop.setBitmap(b);
              info.setText(
                  "Drag to position • pinch to zoom\nThe circle shows your profile picture");
              save.setEnabled(true);
            }
          });
    } catch (final Exception e) {
      failed(e);
    } catch (OutOfMemoryError e) {
      failed(new IOException("Image needs too much memory"));
    } finally {
      if (temp != null) temp.delete();
    }
  }

  private void failed(final Exception e) {
    DebugLog.error("PROFILE_EDIT", e);
    runOnUiThread(
        new Runnable() {
          public void run() {
            if (!closed) {
              info.setText("Cannot process photo. Try a smaller JPEG or PNG.");
              save.setEnabled(crop.bitmap != null);
            }
          }
        });
  }

  protected void onDestroy() {
    closed = true;
    super.onDestroy();
  }

  private static final class CropView extends View {
    Bitmap bitmap;
    float scale, minScale, x, y, side, left, top, lastX, lastY;
    boolean pinch;
    final Paint paint = new Paint(3);
    final ScaleGestureDetector detector;

    CropView(Context c) {
      super(c);
      detector =
          new ScaleGestureDetector(
              c,
              new ScaleGestureDetector.SimpleOnScaleGestureListener() {
                public boolean onScale(ScaleGestureDetector d) {
                  float old = scale;
                  scale = Math.max(minScale, Math.min(minScale * 6, scale * d.getScaleFactor()));
                  float f = scale / old;
                  x = d.getFocusX() - (d.getFocusX() - x) * f;
                  y = d.getFocusY() - (d.getFocusY() - y) * f;
                  clamp();
                  invalidate();
                  return true;
                }
              });
    }

    void setBitmap(Bitmap b) {
      bitmap = b;
      reset();
    }

    protected void onSizeChanged(int w, int h, int ow, int oh) {
      side = Math.max(1, Math.min(w, h) - 32);
      left = (w - side) / 2;
      top = (h - side) / 2;
      reset();
    }

    void reset() {
      if (bitmap == null || side == 0) return;
      minScale = Math.max(side / bitmap.getWidth(), side / bitmap.getHeight());
      scale = minScale;
      x = (getWidth() - bitmap.getWidth() * scale) / 2;
      y = (getHeight() - bitmap.getHeight() * scale) / 2;
      clamp();
      invalidate();
    }

    void clamp() {
      if (bitmap == null) return;
      x = Math.max(left + side - bitmap.getWidth() * scale, Math.min(left, x));
      y = Math.max(top + side - bitmap.getHeight() * scale, Math.min(top, y));
    }

    void rotate() {
      if (bitmap == null) return;
      Matrix m = new Matrix();
      m.setRotate(90);
      Bitmap old = bitmap;
      bitmap = Bitmap.createBitmap(old, 0, 0, old.getWidth(), old.getHeight(), m, true);
      if (old != bitmap) old.recycle();
      reset();
    }

    public boolean onTouchEvent(MotionEvent e) {
      if (bitmap == null) return true;
      detector.onTouchEvent(e);
      int a = e.getActionMasked();
      if (a == MotionEvent.ACTION_DOWN) {
        lastX = e.getX();
        lastY = e.getY();
        pinch = false;
      } else if (a == MotionEvent.ACTION_POINTER_DOWN) pinch = true;
      else if (a == MotionEvent.ACTION_MOVE) {
        if (!pinch && !detector.isInProgress()) {
          x += e.getX() - lastX;
          y += e.getY() - lastY;
          clamp();
          invalidate();
        }
        lastX = e.getX();
        lastY = e.getY();
      } else if (a == MotionEvent.ACTION_UP) {
        performClick();
      }
      return true;
    }

    public boolean performClick() {
      super.performClick();
      return true;
    }

    protected void onDraw(Canvas c) {
      if (bitmap == null) return;
      c.drawColor(0xff172a2b);
      c.drawBitmap(
          bitmap,
          null,
          new RectF(x, y, x + bitmap.getWidth() * scale, y + bitmap.getHeight() * scale),
          paint);
      Path mask = new Path();
      mask.setFillType(Path.FillType.EVEN_ODD);
      mask.addRect(0, 0, getWidth(), getHeight(), Path.Direction.CW);
      mask.addCircle(getWidth() / 2f, getHeight() / 2f, side / 2, Path.Direction.CW);
      paint.setColor(0xaa000000);
      c.drawPath(mask, paint);
      paint.setStyle(Paint.Style.STROKE);
      paint.setStrokeWidth(2);
      paint.setColor(Color.WHITE);
      c.drawCircle(getWidth() / 2f, getHeight() / 2f, side / 2, paint);
      c.drawRect(left, top, left + side, top + side, paint);
      paint.setStyle(Paint.Style.FILL);
    }

    Bitmap result() {
      if (bitmap == null || side <= 0) return null;
      Bitmap b = Bitmap.createBitmap(256, 256, Bitmap.Config.ARGB_8888);
      Canvas c = new Canvas(b);
      c.drawColor(Color.WHITE);
      c.scale(256 / side, 256 / side);
      c.translate(-left, -top);
      c.drawBitmap(
          bitmap,
          null,
          new RectF(x, y, x + bitmap.getWidth() * scale, y + bitmap.getHeight() * scale),
          paint);
      return b;
    }
  }
}
