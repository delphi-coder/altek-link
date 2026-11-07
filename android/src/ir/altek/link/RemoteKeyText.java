package ir.altek.link;

/** PC virtual keys to basic US-layout text for the accessibility input path. */
final class RemoteKeyText {
  static String character(int vk, boolean shift, boolean caps) {
    if (vk >= 0x41 && vk <= 0x5a)
      return String.valueOf((char) ((shift != caps ? 'A' : 'a') + vk - 0x41));
    if (vk >= 0x30 && vk <= 0x39)
      return String.valueOf(shift ? ")!@#$%^&*(".charAt(vk - 0x30) : (char) vk);
    if (vk >= 0x60 && vk <= 0x69) return String.valueOf((char) ('0' + vk - 0x60));
    if (vk == 0x20) return " ";
    String normal = ";=,-./`[\\]'";
    String shifted = ":+<_>?~{|}\"";
    int index;
    switch (vk) {
      case 0xba: index = 0; break;
      case 0xbb: index = 1; break;
      case 0xbc: index = 2; break;
      case 0xbd: index = 3; break;
      case 0xbe: index = 4; break;
      case 0xbf: index = 5; break;
      case 0xc0: index = 6; break;
      case 0xdb: index = 7; break;
      case 0xdc: index = 8; break;
      case 0xdd: index = 9; break;
      case 0xde: index = 10; break;
      default: return null;
    }
    return String.valueOf((shift ? shifted : normal).charAt(index));
  }
}
