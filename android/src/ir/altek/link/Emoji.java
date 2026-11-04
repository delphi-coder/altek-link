package ir.altek.link;

import android.content.Context;
import android.graphics.drawable.Drawable;
import android.text.Spannable;
import android.text.SpannableString;
import android.text.style.ImageSpan;

/** Shared local artwork; the message remains ordinary Unicode on disk and on the wire. */
public final class Emoji {
  public static final String[] TEXT = {"😀", "😂", "🙂", "😍", "👍", "👋", "❤", "🎉", "🙏", "😢"};
  public static final String[] NAMES = {
    "Grin", "Joy", "Smile", "Love", "Thumbs up", "Wave", "Heart", "Celebrate", "Folded hands", "Cry"
  };
  private static final int[] RES = {
    R.drawable.emoji_1f600,
    R.drawable.emoji_1f602,
    R.drawable.emoji_1f642,
    R.drawable.emoji_1f60d,
    R.drawable.emoji_1f44d,
    R.drawable.emoji_1f44b,
    R.drawable.emoji_2764,
    R.drawable.emoji_1f389,
    R.drawable.emoji_1f64f,
    R.drawable.emoji_1f622
  };

  private static final class Icon extends ImageSpan {
    Icon(Drawable d) {
      super(d, ImageSpan.ALIGN_BOTTOM);
    }
  }

  public static CharSequence render(Context c, String text, float size) {
    SpannableString result = new SpannableString(text);
    apply(c, result, size);
    return result;
  }

  public static void apply(Context c, Spannable text, float size) {
    for (Icon old : text.getSpans(0, text.length(), Icon.class)) text.removeSpan(old);
    String value = text.toString();
    for (int i = 0; i < TEXT.length; i++) {
      int start = 0;
      while ((start = value.indexOf(TEXT[i], start)) >= 0) {
        int end = start + TEXT[i].length();
        if (end < value.length() && value.charAt(end) == '\ufe0f') end++;
        // Do not replace one component of a skin-tone or ZWJ emoji sequence.
        int next = end < value.length() ? Character.codePointAt(value, end) : -1;
        if (next != 0x200d
            && !(next >= 0x1f3fb && next <= 0x1f3ff)
            && !(start > 0 && value.charAt(start - 1) == '\u200d')) {
          Drawable drawable = c.getResources().getDrawable(RES[i]);
          int pixels = Math.max(1, Math.round(size * 1.2f));
          drawable.setBounds(0, 0, pixels, pixels);
          text.setSpan(new Icon(drawable), start, end, Spannable.SPAN_EXCLUSIVE_EXCLUSIVE);
        }
        start = end;
      }
    }
  }
}
