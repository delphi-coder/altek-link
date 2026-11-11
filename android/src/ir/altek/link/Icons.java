package ir.altek.link;

import android.graphics.*;
import android.graphics.drawable.Drawable;

/** Small vector-like native icons, compatible with API16 without font glyph dependencies. */
public final class Icons extends Drawable {
  private final String kind;
  private final Paint p = new Paint(3);
  private final int color;

  public Icons(String kind, int color) {
    this.kind = kind;
    this.color = color;
  }

  public int getIntrinsicWidth() {
    return 24;
  }

  public int getIntrinsicHeight() {
    return 24;
  }

  public void draw(Canvas c) {
    c.save();
    Rect b = getBounds();
    c.translate(b.left, b.top);
    c.scale(b.width() / 24f, b.height() / 24f);
    p.setColor(color);
    p.setStrokeWidth(2);
    p.setStyle(Paint.Style.STROKE);
    p.setStrokeCap(Paint.Cap.ROUND);
    p.setStrokeJoin(Paint.Join.ROUND);
    if (kind.equals("keyboard")) {
      c.drawRoundRect(new RectF(2, 5, 22, 19), 2, 2, p);
      p.setStrokeWidth(1.5f);
      for (int x = 6; x <= 18; x += 4) {
        c.drawPoint(x, 9, p);
        c.drawPoint(x, 12, p);
      }
      c.drawLine(8, 16, 16, 16, p);
    } else if (kind.equals("video")) {
      c.drawRoundRect(new RectF(2, 6, 16, 18), 2, 2, p);
      Path v = new Path();
      v.moveTo(16, 10);
      v.lineTo(22, 6);
      v.lineTo(22, 18);
      v.lineTo(16, 14);
      c.drawPath(v, p);
    } else if (kind.equals("audio")) {
      Path v = new Path();
      v.moveTo(5, 3);
      v.lineTo(9, 7);
      v.lineTo(7, 10);
      v.quadTo(10, 15, 14, 17);
      v.lineTo(17, 15);
      v.lineTo(21, 19);
      v.lineTo(19, 22);
      v.cubicTo(10, 23, 1, 13, 2, 5);
      v.close();
      c.drawPath(v, p);
    } else if (kind.equals("back")) {
      c.drawLine(19, 12, 5, 12, p);
      c.drawLine(5, 12, 11, 6, p);
      c.drawLine(5, 12, 11, 18, p);
    } else if (kind.equals("menu")) {
      p.setStyle(Paint.Style.FILL);
      for (int y = 5; y <= 19; y += 7) c.drawCircle(12, y, 1.5f, p);
    } else if (kind.equals("send")) {
      Path v = new Path();
      v.moveTo(3, 3);
      v.lineTo(22, 12);
      v.lineTo(3, 21);
      v.lineTo(6, 12);
      v.close();
      c.drawPath(v, p);
      c.drawLine(6, 12, 21, 12, p);
    } else if (kind.equals("attach")) {
      Path clip = new Path();
      clip.moveTo(12, 8);
      clip.lineTo(12, 17);
      clip.cubicTo(12, 19, 15, 19, 15, 17);
      clip.lineTo(15, 6);
      clip.cubicTo(15, 2, 9, 2, 9, 6);
      clip.lineTo(9, 17);
      clip.cubicTo(9, 23, 18, 23, 18, 17);
      clip.lineTo(18, 8);
      c.drawPath(clip, p);
    } else if (kind.equals("emoji")) {
      c.drawCircle(12, 12, 9, p);
      c.drawPoint(9, 9, p);
      c.drawPoint(15, 9, p);
      c.drawArc(new RectF(7, 9, 17, 17), 10, 160, false, p);
    } else if (kind.equals("flip")) {
      c.drawRoundRect(new RectF(3, 6, 21, 19), 2, 2, p);
      c.drawLine(8, 6, 10, 3, p);
      c.drawLine(10, 3, 15, 3, p);
      c.drawLine(15, 3, 17, 6, p);
      c.drawArc(new RectF(7, 8, 17, 17), -70, 260, false, p);
      c.drawLine(7, 10, 7, 14, p);
      c.drawLine(7, 10, 11, 10, p);
    } else if (kind.equals("torch")) {
      Path v = new Path();
      v.moveTo(14, 2);
      v.lineTo(5, 14);
      v.lineTo(11, 14);
      v.lineTo(10, 22);
      v.lineTo(20, 9);
      v.lineTo(13, 9);
      v.close();
      c.drawPath(v, p);
    } else if (kind.equals("speaker")) {
      Path v = new Path();
      v.moveTo(3, 9);
      v.lineTo(7, 9);
      v.lineTo(12, 5);
      v.lineTo(12, 19);
      v.lineTo(7, 15);
      v.lineTo(3, 15);
      v.close();
      c.drawPath(v, p);
      c.drawArc(new RectF(11, 7, 19, 17), -65, 130, false, p);
      c.drawArc(new RectF(10, 3, 24, 21), -65, 130, false, p);
    } else if (kind.equals("end")) {
      Path v = new Path();
      v.moveTo(3, 16);
      v.lineTo(3, 12);
      v.quadTo(12, 4, 21, 12);
      v.lineTo(21, 16);
      v.lineTo(16, 16);
      v.lineTo(16, 12);
      v.quadTo(12, 10, 8, 12);
      v.lineTo(8, 16);
      v.close();
      c.drawPath(v, p);
    } else if (kind.equals("muted")) {
      c.drawRoundRect(new RectF(9, 2, 15, 15), 3, 3, p);
      c.drawArc(new RectF(5, 7, 19, 19), 0, 180, false, p);
      c.drawLine(12, 19, 12, 23, p);
      c.drawLine(3, 3, 21, 21, p);
    } else if (kind.equals("stop")) {
      p.setStyle(Paint.Style.FILL);
      c.drawRect(5, 5, 19, 19, p);
    } else if (kind.equals("home")) {
      Path v = new Path();
      v.moveTo(4, 12);
      v.lineTo(12, 4);
      v.lineTo(20, 12);
      c.drawPath(v, p);
      c.drawRect(6.5f, 12, 17.5f, 20, p);
      c.drawLine(12, 20, 12, 15, p);
    } else if (kind.equals("recents")) {
      c.drawRoundRect(new RectF(4, 7, 15, 18), 2, 2, p);
      c.drawRoundRect(new RectF(9, 3, 20, 14), 2, 2, p);
    } else if (kind.equals("fit")) {
      p.setStrokeWidth(2);
      c.drawLine(6, 3, 3, 3, p);
      c.drawLine(3, 3, 3, 6, p);
      c.drawLine(18, 3, 21, 3, p);
      c.drawLine(21, 3, 21, 6, p);
      c.drawLine(6, 21, 3, 21, p);
      c.drawLine(3, 21, 3, 18, p);
      c.drawLine(18, 21, 21, 21, p);
      c.drawLine(21, 21, 21, 18, p);
    } else if (kind.equals("rotate")) {
      p.setStrokeWidth(2);
      c.drawArc(new RectF(4, 5, 20, 21), 20, 300, false, p);
      c.drawLine(20, 5, 20, 10, p);
      c.drawLine(20, 5, 15, 5, p);
    } else if (kind.equals("zoom")) {
      p.setStrokeWidth(2);
      c.drawCircle(9.5f, 9.5f, 5.5f, p);
      c.drawLine(13.5f, 13.5f, 20, 20, p);
      c.drawLine(6.5f, 9.5f, 12.5f, 9.5f, p);
      c.drawLine(9.5f, 6.5f, 9.5f, 12.5f, p);
    } else {
      c.drawRoundRect(new RectF(9, 2, 15, 15), 3, 3, p);
      c.drawArc(new RectF(5, 7, 19, 19), 0, 180, false, p);
      c.drawLine(12, 19, 12, 23, p);
      c.drawLine(8, 23, 16, 23, p);
    }
    c.restore();
  }

  public void setAlpha(int a) {
    p.setAlpha(a);
  }

  public void setColorFilter(ColorFilter f) {
    p.setColorFilter(f);
  }

  public int getOpacity() {
    return PixelFormat.TRANSLUCENT;
  }
}
