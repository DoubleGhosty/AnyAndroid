package aa.host;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.ConnectException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketException;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/**
 * Self-test of libcore's sockets (G2) on AOSP's real libcore, through java.net as an application
 * uses it, on the loopback interface only: name lookups, TCP between two threads, UDP, options,
 * timeouts, a refused connection and a close while another thread is blocked. The application
 * declares android.permission.INTERNET. Each line is what Android prints.
 */
public final class NetProbe {
  private NetProbe() {}

  interface Check {
    String run() throws Throwable;
  }

  static void check(String what, Check c) {
    String result;
    try {
      result = c.run();
    } catch (Throwable t) {
      result = "échec : " + t.getClass().getName();  // messages differ from ART's
    }
    System.out.println(what + " : " + result);
  }

  static final InetAddress LOOPBACK = loopback();

  static InetAddress loopback() {
    try {
      return InetAddress.getByAddress(new byte[] {127, 0, 0, 1});
    } catch (IOException e) {
      throw new AssertionError(e);
    }
  }

  public static void main(String[] args) {
    check("localhost", () -> InetAddress.getByName("localhost") + ", " + InetAddress.getAllByName("localhost").length
        + " adresse, nom inverse " + LOOPBACK.getHostName());
    check("TCP en boucle locale", NetProbe::tcp);
    check("options de socket", () -> {
      try (Socket s = new Socket()) {
        s.setTcpNoDelay(true);
        s.setKeepAlive(true);
        s.setSoLinger(true, 5);
        s.setReuseAddress(true);
        s.setSoTimeout(1500);
        return "TCP_NODELAY " + s.getTcpNoDelay() + ", SO_KEEPALIVE " + s.getKeepAlive() + ", SO_LINGER "
            + s.getSoLinger() + ", SO_REUSEADDR " + s.getReuseAddress() + ", SO_TIMEOUT " + s.getSoTimeout();
      }
    });
    check("délai de lecture", () -> {
      try (ServerSocket server = new ServerSocket(0, 1, LOOPBACK);
           Socket client = new Socket(LOOPBACK, server.getLocalPort());
           Socket peer = server.accept()) {
        client.setSoTimeout(200);
        long start = System.nanoTime();
        try {
          client.getInputStream().read();
          return "lu";
        } catch (SocketTimeoutException e) {
          return "SocketTimeoutException après au moins 200 ms : " + (System.nanoTime() - start >= 200_000_000L);
        }
      }
    });
    check("connexion refusée", () -> {
      int port;
      try (ServerSocket closed = new ServerSocket(0, 1, LOOPBACK)) {
        port = closed.getLocalPort();
      }
      try (Socket s = new Socket()) {
        s.connect(new InetSocketAddress(LOOPBACK, port));
        return "connecté";
      } catch (ConnectException e) {
        return e.getClass().getSimpleName() + ", ECONNREFUSED " + e.getMessage().contains("ECONNREFUSED (Connection refused)");
      }
    });
    check("connexion avec délai", () -> {
      try (ServerSocket server = new ServerSocket(0, 1, LOOPBACK); Socket s = new Socket()) {
        s.connect(new InetSocketAddress(LOOPBACK, server.getLocalPort()), 2000);
        try (Socket peer = server.accept()) {
          return "connecté " + s.isConnected() + ", pair " + (peer.getPort() == s.getLocalPort());
        }
      }
    });
    check("fermeture pendant accept", () -> {
      ServerSocket server = new ServerSocket(0, 1, LOOPBACK);
      String[] seen = new String[1];
      Thread waiter = new Thread(() -> {
        try {
          server.accept();
          seen[0] = "accepté";
        } catch (IOException e) {
          seen[0] = e.getClass().getName() + ": " + e.getMessage();
        }
      });
      waiter.start();
      while (waiter.getState() == Thread.State.NEW) Thread.sleep(1);
      Thread.sleep(100);  // the waiter is in poll(2)
      server.close();
      waiter.join(5000);
      return seen[0] + ", fil terminé " + !waiter.isAlive();
    });
    check("UDP en boucle locale", () -> {
      try (DatagramSocket a = new DatagramSocket(0, LOOPBACK); DatagramSocket b = new DatagramSocket(0, LOOPBACK)) {
        byte[] out = "datagramme".getBytes(StandardCharsets.UTF_8);
        a.send(new DatagramPacket(out, out.length, LOOPBACK, b.getLocalPort()));
        byte[] in = new byte[64];
        DatagramPacket p = new DatagramPacket(in, in.length);
        b.setSoTimeout(2000);
        b.receive(p);
        byte[] small = new byte[4];
        a.send(new DatagramPacket(out, out.length, LOOPBACK, b.getLocalPort()));
        DatagramPacket truncated = new DatagramPacket(small, small.length);
        b.receive(truncated);
        return new String(in, 0, p.getLength(), StandardCharsets.UTF_8) + " de " + p.getAddress()
            + ", port de l'émetteur " + (p.getPort() == a.getLocalPort()) + ", tronqué à " + truncated.getLength() + " : "
            + new String(small, StandardCharsets.UTF_8);
      }
    });
  }

  // A server thread answers each line in upper case; the client half-closes and sees the end.
  static String tcp() throws Exception {
    try (ServerSocket server = new ServerSocket(0, 50, LOOPBACK)) {
      String[] seen = new String[2];
      Thread serverThread = new Thread(() -> {
        try (Socket peer = server.accept()) {
          BufferedReader in = new BufferedReader(new InputStreamReader(peer.getInputStream(), StandardCharsets.UTF_8));
          OutputStream out = peer.getOutputStream();
          String line;
          int lines = 0;
          while ((line = in.readLine()) != null) {
            out.write((line.toUpperCase() + "\n").getBytes(StandardCharsets.UTF_8));
            lines++;
          }
          seen[0] = lines + " lignes puis fin de flux";
          seen[1] = String.valueOf(peer.getInetAddress());
        } catch (IOException e) {
          seen[0] = "serveur : " + e;
        }
      });
      serverThread.start();
      try (Socket client = new Socket(LOOPBACK, server.getLocalPort())) {
        OutputStream out = client.getOutputStream();
        BufferedReader in = new BufferedReader(new InputStreamReader(client.getInputStream(), StandardCharsets.UTF_8));
        out.write("bonjour\n".getBytes(StandardCharsets.UTF_8));
        String first = in.readLine();
        byte[] big = new byte[256 * 1024];
        Arrays.fill(big, (byte) 'x');
        big[big.length - 1] = '\n';
        out.write(big);
        int echoed = in.readLine().length();
        client.shutdownOutput();
        int end = client.getInputStream().read();
        serverThread.join(10000);
        return first + ", " + echoed + " octets en écho, fin " + end + ", " + seen[0] + ", pair " + seen[1] + ", ports "
            + (client.getPort() == server.getLocalPort()) + ", local " + client.getLocalAddress();
      }
    }
  }
}
