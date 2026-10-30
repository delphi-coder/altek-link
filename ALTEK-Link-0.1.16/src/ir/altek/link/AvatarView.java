package ir.altek.link;

import android.content.Context;
import android.graphics.*;
import android.view.View;

public final class AvatarView extends View {
  private String peer = "self", name = "?";
  private final Paint paint = new Paint(3);

  public AvatarView(Context c) {
    super(c);
    setLayerType(View.LAYER_TYPE_SOFTWARE, null);
  }

  public void bind(String id, String label) {
    peer = id;
    name = label;
    setContentDescription(label);
    invalidate();
  }

  protected void onDraw(Canvas c) {
    float r = Math.min(getWidth(), getHeight()) / 2f, x = getWidth() / 2f, y = getHeight() / 2f;
    Bitmap b = Profile.bitmap(getContext(), peer);
    if (b != null) {
      BitmapShader shader = new BitmapShader(b, Shader.TileMode.CLAMP, Shader.TileMode.CLAMP);
      Matrix m = new Matrix();
      m.setScale(2 * r / b.getWidth(), 2 * r / b.getHeight());
      m.postTranslate(x - r, y - r);
      shader.setLocalMatrix(m);
      paint.setShader(shader);
      c.drawCircle(x, y, r, paint);
      paint.setShader(null);
    } else {
      paint.setColor(Color.rgb(25, 132, 125));
      c.drawCircle(x, y, r, paint);
      paint.setColor(Color.WHITE);
      paint.setTextSize(r * .9f);
      paint.setTextAlign(Paint.Align.CENTER);
      String initial =
          name.length() == 0
              ? "?"
              : name.substring(0, Character.charCount(name.codePointAt(0))).toUpperCase();
      c.drawText(initial, x, y - (paint.ascent() + paint.descent()) / 2, paint);
    }
  }
}
