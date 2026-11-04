package ir.altek.link;

import java.io.*;
import java.net.*;
import java.util.*;
import java.util.concurrent.*;
import javax.net.ssl.*;

public final class CoreTest {
  static void check(boolean yes, String name) {
    if (!yes) throw new AssertionError(name);
    System.out.println("PASS " + name);
  }

  public static void main(String[] args) throws Exception {
    String text = "سلام فارسی 👋 😀 abc 123";
    ByteArrayOutputStream b = new ByteArrayOutputStream();
    DataOutputStream o = new DataOutputStream(b);
    Wire.text(o, text);
    Wire.text(o, "second");
    DataInputStream in = new DataInputStream(new ByteArrayInputStream(b.toByteArray()));
    check(
        Wire.readText(in).equals(text) && Wire.readText(in).equals("second"),
        "UTF8 emoji and consecutive frame boundaries");
    byte[] profile = new byte[54000];
    new java.util.Random(12).nextBytes(profile);
    ByteArrayOutputStream profileFrame = new ByteArrayOutputStream();
    Wire.write(new DataOutputStream(profileFrame), profile);
    check(Arrays.equals(profile, Wire.read(new DataInputStream(new ByteArrayInputStream(profileFrame.toByteArray())), 65536)), "Profile-sized control frame round trip");
    boolean bad = false;
    try {
      Wire.read(new DataInputStream(new ByteArrayInputStream(new byte[] {127, -1, -1, -1})), 1024);
    } catch (IOException e) {
      bad = true;
    }
    check(bad, "Oversized frame rejected before allocation");
    bad = false;
    try {
      Wire.read(new DataInputStream(new ByteArrayInputStream(new byte[] {-1, -1, -1, -1})), 1024);
    } catch (IOException e) {
      bad = true;
    }
    check(bad, "Negative frame length rejected");
    bad = false;
    try {
      Wire.read(new DataInputStream(new ByteArrayInputStream(new byte[] {0, 0, 0, 4, 1})), 1024);
    } catch (EOFException e) {
      bad = true;
    }
    check(bad, "Truncated frame rejected");
    File root = new File(args[0]);
    Identity a = new Identity(new File(root, "a")), other = new Identity(new File(root, "b"));
    Identity reload = new Identity(new File(root, "a"));
    check(
        a.id.equals(reload.id) && !a.id.equals(other.id),
        "Persistent and distinct device identities");
    final SSLServerSocket server =
        (SSLServerSocket) a.tls.getServerSocketFactory().createServerSocket(0);
    server.setEnabledProtocols(new String[] {"TLSv1.2"});
    server.setNeedClientAuth(true);
    final String expected = other.id;
    ExecutorService ex = Executors.newSingleThreadExecutor();
    Future<String> received =
        ex.submit(
            new Callable<String>() {
              public String call() throws Exception {
                SSLSocket s = (SSLSocket) server.accept();
                s.setSoTimeout(5000);
                try {
                  s.startHandshake();
                  if (!Identity.peer(s).equals(expected))
                    throw new AssertionError("Client identity");
                  String msg = Wire.readText(new DataInputStream(s.getInputStream()));
                  Wire.text(new DataOutputStream(s.getOutputStream()), msg);
                  return msg;
                } finally {
                  s.close();
                }
              }
            });
    SSLSocket client =
        (SSLSocket) other.tls.getSocketFactory().createSocket("127.0.0.1", server.getLocalPort());
    client.setEnabledProtocols(new String[] {"TLSv1.2"});
    client.setSoTimeout(5000);
    client.startHandshake();
    check(Identity.peer(client).equals(a.id), "TLS peer certificate pin");
    Wire.text(new DataOutputStream(client.getOutputStream()), text);
    check(
        Wire.readText(new DataInputStream(client.getInputStream())).equals(text)
            && received.get(6, TimeUnit.SECONDS).equals(text),
        "Mutual TLS 1.2 Unicode round trip");
    client.close();
    server.close();
    ex.shutdownNow();
    byte[] src = {1, 2, 3, 4, 5, 6, 7, 8, 91, 81, 92, 82}, out = new byte[12];
    VideoCapture.convert(src, out, 4, 2, true);
    check(
        Arrays.equals(out, new byte[] {1, 2, 3, 4, 5, 6, 7, 8, 81, 82, 91, 92}), "NV21 to planar");
    VideoCapture.convert(src, out, 4, 2, false);
    check(
        Arrays.equals(out, new byte[] {1, 2, 3, 4, 5, 6, 7, 8, 81, 91, 82, 92}),
        "NV21 to semiplanar");
    VideoCapture.convert(src,out,4,2,true,true);
    check(Arrays.equals(out,new byte[]{8,7,6,5,4,3,2,1,82,81,92,91}),"Front half-turn planar luma/chroma");
    VideoCapture.convert(src,out,4,2,false,true);
    check(Arrays.equals(out,new byte[]{8,7,6,5,4,3,2,1,82,92,81,91}),"Front half-turn semiplanar luma/chroma");
    byte[] square=new byte[24], rotated=new byte[24];for(int i=0;i<24;i++)square[i]=(byte)i;
    VideoCapture.convert(square,rotated,4,4,false,true);
    check(Arrays.equals(rotated,new byte[]{15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,23,22,21,20,19,18,17,16}),"Half-turn reverses both chroma rows without swapping color channels");

  }
}
