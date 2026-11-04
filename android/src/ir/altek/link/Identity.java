package ir.altek.link;

import java.io.*;
import java.math.BigInteger;
import java.security.*;
import java.security.cert.*;
import java.security.spec.PKCS8EncodedKeySpec;
import java.util.*;
import javax.net.ssl.*;

/**
 * Per-install RSA identity + self-signed X.509; TLS handles encryption, app pins SHA-256
 * certificate IDs. Self-signed certificates establish a key, NOT trust: every operation authorizes
 * that ID in Store. DER below encodes a certificate only; it is not a custom encryption protocol.
 */
public final class Identity {
  public final String id;
  public final SSLContext tls;

  public Identity(File dir) throws Exception {
    dir.mkdirs();
    File key = new File(dir, "identity.pk8"), cert = new File(dir, "identity.der");
    KeyFactory factory = KeyFactory.getInstance("RSA");
    PrivateKey privateKey;
    X509Certificate certificate;
    if (key.exists() && cert.exists()) {
      privateKey = factory.generatePrivate(new PKCS8EncodedKeySpec(bytes(key)));
      certificate =
          (X509Certificate)
              CertificateFactory.getInstance("X.509")
                  .generateCertificate(new ByteArrayInputStream(bytes(cert)));
    } else {
      KeyPairGenerator g = KeyPairGenerator.getInstance("RSA");
      g.initialize(2048, new SecureRandom());
      KeyPair pair = g.generateKeyPair();
      privateKey = pair.getPrivate();
      byte[] encoded = certificate(pair);
      certificate =
          (X509Certificate)
              CertificateFactory.getInstance("X.509")
                  .generateCertificate(new ByteArrayInputStream(encoded));
      certificate.verify(pair.getPublic());
      save(key, privateKey.getEncoded());
      save(cert, encoded);
    }
    id = Wire.digest(certificate.getEncoded());
    KeyStore ks = KeyStore.getInstance(KeyStore.getDefaultType());
    ks.load(null, null);
    char[] pass = "local-key-entry".toCharArray();
    ks.setKeyEntry(
        "identity", privateKey, pass, new java.security.cert.Certificate[] {certificate});
    KeyManagerFactory km = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm());
    km.init(ks, pass);
    X509TrustManager identityOnly =
        new X509TrustManager() {
          public X509Certificate[] getAcceptedIssuers() {
            return new X509Certificate[0];
          }

          private void check(X509Certificate[] chain) throws CertificateException {
            if (chain == null || chain.length != 1)
              throw new CertificateException("Expected self-signed identity");
            try {
              chain[0].verify(chain[0].getPublicKey());
              if (!"RSA".equals(chain[0].getPublicKey().getAlgorithm())
                  || ((java.security.interfaces.RSAPublicKey) chain[0].getPublicKey())
                          .getModulus()
                          .bitLength()
                      < 2048) throw new Exception("Weak key");
            } catch (Exception e) {
              throw new CertificateException(e);
            }
          }

          public void checkClientTrusted(X509Certificate[] c, String a)
              throws CertificateException {
            check(c);
          }

          public void checkServerTrusted(X509Certificate[] c, String a)
              throws CertificateException {
            check(c);
          }
        };
    tls = SSLContext.getInstance("TLSv1.2");
    tls.init(km.getKeyManagers(), new TrustManager[] {identityOnly}, new SecureRandom());
  }

  public static String peer(SSLSocket socket) throws Exception {
    return Wire.digest(socket.getSession().getPeerCertificates()[0].getEncoded());
  }

  public SSLSocket connect(String ip) throws Exception {
    SSLSocket s = (SSLSocket) tls.getSocketFactory().createSocket();
    try {
      s.setEnabledProtocols(new String[] {"TLSv1.2"});
      s.connect(new java.net.InetSocketAddress(ip, Wire.PORT), 4000);
      s.setSoTimeout(12000);
      s.setTcpNoDelay(true);
      s.startHandshake();
      return s;
    } catch (Exception e) {
      Wire.close(s);
      throw e;
    }
  }

  static byte[] bytes(File f) throws IOException {
    FileInputStream in = new FileInputStream(f);
    try {
      ByteArrayOutputStream b = new ByteArrayOutputStream();
      byte[] a = new byte[4096];
      int n;
      while ((n = in.read(a)) != -1) b.write(a, 0, n);
      return b.toByteArray();
    } finally {
      in.close();
    }
  }

  static void save(File f, byte[] b) throws IOException {
    File temp = new File(f.getPath() + ".tmp");
    FileOutputStream out = new FileOutputStream(temp);
    try {
      out.write(b);
      out.getFD().sync();
    } finally {
      out.close();
    }
    if (!temp.renameTo(f)) throw new IOException("Identity save failed");
  }

  static byte[] der(int tag, byte[] value) throws IOException {
    ByteArrayOutputStream o = new ByteArrayOutputStream();
    o.write(tag);
    int n = value.length;
    if (n < 128) o.write(n);
    else {
      int count = 0;
      for (int x = n; x > 0; x >>>= 8) count++;
      o.write(128 | count);
      for (int i = count - 1; i >= 0; i--) o.write((n >>> (i * 8)) & 255);
    }
    o.write(value);
    return o.toByteArray();
  }

  static byte[] join(byte[]... xs) throws IOException {
    ByteArrayOutputStream o = new ByteArrayOutputStream();
    for (byte[] b : xs) o.write(b);
    return o.toByteArray();
  }

  static byte[] seq(byte[]... xs) throws IOException {
    return der(48, join(xs));
  }

  static byte[] certificate(KeyPair key) throws Exception {
    byte[] alg =
        seq(
            der(6, new byte[] {42, (byte) 134, 72, (byte) 134, (byte) 247, 13, 1, 1, 11}),
            der(5, new byte[0]));
    byte[] name =
        seq(
            der(
                49,
                seq(
                    der(6, new byte[] {85, 4, 3}),
                    der(12, "ALTEK Link device".getBytes("UTF-8")))));
    // Fixed validity avoids first-run clock skew. Trust is a pinned local key, not a public CA
    // name.
    byte[] validity =
        seq(
            der(23, "200101000000Z".getBytes("US-ASCII")),
            der(23, "491231235959Z".getBytes("US-ASCII")));
    byte[] serial = new BigInteger(120, new SecureRandom()).add(BigInteger.ONE).toByteArray();
    byte[] tbs = seq(der(2, serial), alg, name, validity, name, key.getPublic().getEncoded());
    Signature sign = Signature.getInstance("SHA256withRSA");
    sign.initSign(key.getPrivate());
    sign.update(tbs);
    return seq(tbs, alg, der(3, join(new byte[] {0}, sign.sign())));
  }
}
