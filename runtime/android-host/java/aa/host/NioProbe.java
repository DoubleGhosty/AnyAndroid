package aa.host;

import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import java.io.FileDescriptor;
import java.io.IOException;
import java.io.InputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.SocketTimeoutException;
import java.nio.ByteBuffer;
import java.nio.channels.DatagramChannel;
import java.nio.channels.Pipe;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.nio.channels.ServerSocketChannel;
import java.nio.channels.SocketChannel;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;

/**
 * Self-test of libcore's NIO channels (G2) on AOSP's real libcore, on the loopback interface
 * only: pipes, blocking and non-blocking socket channels, the selector (poll), its wakeup, a close
 * and an interrupt while another thread is blocked, datagram channels and the socket adaptor.
 * The application declares android.permission.INTERNET. Each line is what Android prints.
 */
public final class NioProbe {
  private NioProbe() {}

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

  static ByteBuffer ascii(String s) {
    return ByteBuffer.wrap(s.getBytes(StandardCharsets.US_ASCII));
  }

  static String text(ByteBuffer b) {
    b.flip();
    return StandardCharsets.US_ASCII.decode(b).toString();
  }

  // Lets `t` start and reach its blocking system call (a thread in a native wait stays RUNNABLE).
  static void awaitBlocked(Thread t) throws InterruptedException {
    while (t.getState() == Thread.State.NEW) Thread.sleep(1);
    Thread.sleep(200);
  }

  public static void main(String[] args) {
    check("tube NIO", () -> {
      Pipe pipe = Pipe.open();
      try (Pipe.SinkChannel sink = pipe.sink(); Pipe.SourceChannel source = pipe.source()) {
        sink.write(ascii("tube"));
        ByteBuffer in = ByteBuffer.allocate(16);
        source.read(in);
        source.configureBlocking(false);
        int empty = source.read(ByteBuffer.allocate(4));
        sink.close();
        int end = source.read(ByteBuffer.allocate(4));
        return text(in) + ", vide " + empty + ", fin " + end;
      }
    });
    check("pipe2", () -> {
      FileDescriptor[] fds = Os.pipe2(OsConstants.O_NONBLOCK);
      try {
        String again;
        try {
          Os.read(fds[0], new byte[4], 0, 4);
          again = "lu";
        } catch (ErrnoException e) {
          again = e.errno == OsConstants.EAGAIN ? "EAGAIN" : e.getMessage();
        }
        byte[] out = "abc".getBytes(StandardCharsets.US_ASCII);
        int written = Os.write(fds[1], out, 0, out.length);
        byte[] in = new byte[8];
        int read = Os.read(fds[0], in, 0, in.length);
        String wrongEnd;
        try {
          Os.write(fds[0], out, 0, 1);
          wrongEnd = "écrit";
        } catch (ErrnoException e) {
          wrongEnd = e.getMessage();
        }
        Os.close(fds[1]);
        int end = Os.read(fds[0], in, 0, in.length);
        return again + ", " + written + " écrits, " + new String(in, 0, read, StandardCharsets.US_ASCII) + ", " + wrongEnd
            + ", fin " + end;
      } finally {
        Os.close(fds[0]);
      }
    });
    check("canaux bloquants", NioProbe::blocking);
    check("sélecteur", NioProbe::selector);
    check("réveil du sélecteur", () -> {
      try (Selector selector = Selector.open()) {
        long start = System.nanoTime();
        int none = selector.select(100);
        boolean waited = System.nanoTime() - start >= 100_000_000L;
        Thread waker = new Thread(() -> {
          try {
            Thread.sleep(100);
          } catch (InterruptedException e) {
            return;
          }
          selector.wakeup();
        });
        waker.start();
        int woken = selector.select();
        waker.join();
        selector.wakeup();
        int now = selector.selectNow();  // a wakeup before selectNow is consumed by it
        int after = selector.select(50);
        return "délai " + none + " après 100 ms : " + waited + ", réveillé " + woken + ", selectNow " + now + ", puis " + after;
      }
    });
    check("fermeture pendant read", () -> blockedRead(false));
    check("interruption pendant read", () -> blockedRead(true));
    check("datagrammes", () -> {
      try (DatagramChannel a = DatagramChannel.open(); DatagramChannel b = DatagramChannel.open()) {
        a.bind(new InetSocketAddress(LOOPBACK, 0));
        b.bind(new InetSocketAddress(LOOPBACK, 0));
        a.send(ascii("datagramme"), b.getLocalAddress());
        ByteBuffer in = ByteBuffer.allocate(64);
        InetSocketAddress from = (InetSocketAddress) b.receive(in);
        b.configureBlocking(false);
        Object nothing = b.receive(ByteBuffer.allocate(8));
        a.connect(b.getLocalAddress());
        a.write(ascii("connecte"));
        b.configureBlocking(true);
        ByteBuffer small = ByteBuffer.allocate(4);
        b.receive(small);
        return text(in) + " de " + from.getAddress() + ", port " + (from.getPort() == ((InetSocketAddress) a.getLocalAddress()).getPort())
            + ", rien " + nothing + ", connecté " + a.isConnected() + ", tronqué " + text(small);
      }
    });
    check("adaptateur de socket", () -> {
      try (ServerSocketChannel server = ServerSocketChannel.open()) {
        server.bind(new InetSocketAddress(LOOPBACK, 0));
        try (SocketChannel client = SocketChannel.open(server.getLocalAddress()); SocketChannel peer = server.accept()) {
          client.socket().setSoTimeout(200);
          InputStream in = client.socket().getInputStream();
          String timeout;
          try {
            in.read();
            timeout = "lu";
          } catch (SocketTimeoutException e) {
            timeout = "SocketTimeoutException";
          }
          peer.write(ascii("Z"));
          return timeout + ", puis " + (char) in.read() + ", TCP_NODELAY " + client.socket().getTcpNoDelay();
        }
      }
    });
  }

  // A server thread echoes in upper case what it reads, until the end of the stream.
  static String blocking() throws Exception {
    try (ServerSocketChannel server = ServerSocketChannel.open()) {
      server.bind(new InetSocketAddress(LOOPBACK, 0), 10);
      InetSocketAddress where = (InetSocketAddress) server.getLocalAddress();
      String[] seen = new String[1];
      Thread echo = new Thread(() -> {
        try (SocketChannel peer = server.accept()) {
          ByteBuffer buf = ByteBuffer.allocate(4096);
          long total = 0;
          while (peer.read(buf) >= 0) {
            buf.flip();
            for (int i = buf.position(); i < buf.limit(); i++) buf.put(i, (byte) Character.toUpperCase(buf.get(i)));
            total += buf.remaining();
            while (buf.hasRemaining()) peer.write(buf);
            buf.clear();
          }
          seen[0] = total + " octets puis fin";
        } catch (IOException e) {
          seen[0] = "serveur : " + e;
        }
      });
      echo.start();
      try (SocketChannel client = SocketChannel.open(where)) {
        client.write(ascii("bonjour"));
        ByteBuffer first = ByteBuffer.allocate(7);
        while (first.hasRemaining()) client.read(first);
        ByteBuffer big = ByteBuffer.allocateDirect(200_000);
        while (big.hasRemaining()) big.put((byte) 'x');
        big.flip();
        long[] back = new long[1];
        Thread reader = new Thread(() -> {
          ByteBuffer in = ByteBuffer.allocate(8192);
          try {
            while (back[0] < 200_000) {
              int n = client.read(in);
              if (n < 0) break;
              back[0] += n;
              in.clear();
            }
          } catch (IOException e) {
            back[0] = -1;
          }
        });
        reader.start();
        while (big.hasRemaining()) client.write(big);
        reader.join(20000);
        client.shutdownOutput();
        int end = client.read(ByteBuffer.allocate(1));
        echo.join(10000);
        return text(first) + ", " + back[0] + " octets en écho, fin " + end + ", " + seen[0] + ", local "
            + ((InetSocketAddress) client.getLocalAddress()).getAddress() + ", pair " + client.getRemoteAddress().equals(where);
      }
    }
  }

  // Accept, connect, read and write readiness through one selector, the channels non-blocking.
  static String selector() throws Exception {
    List<String> events = new ArrayList<>();
    try (Selector selector = Selector.open(); ServerSocketChannel server = ServerSocketChannel.open();
         SocketChannel client = SocketChannel.open()) {
      server.bind(new InetSocketAddress(LOOPBACK, 0));
      server.configureBlocking(false);
      server.register(selector, SelectionKey.OP_ACCEPT);
      client.configureBlocking(false);
      // A loopback connection may be made at once; then there is no OP_CONNECT to wait for.
      if (client.connect(server.getLocalAddress())) {
        events.add("connect true");
        client.register(selector, SelectionKey.OP_WRITE);
      } else {
        client.register(selector, SelectionKey.OP_CONNECT);
      }
      SocketChannel peer = null;
      String received = null;
      long deadline = System.currentTimeMillis() + 10000;
      while (received == null && System.currentTimeMillis() < deadline) {
        if (selector.select(1000) == 0) continue;
        Iterator<SelectionKey> it = selector.selectedKeys().iterator();
        while (it.hasNext()) {
          SelectionKey key = it.next();
          it.remove();
          if (key.isAcceptable()) {
            events.add("accept");
            peer = server.accept();
            peer.configureBlocking(false);
            peer.register(selector, SelectionKey.OP_READ);
          } else if (key.isConnectable()) {
            events.add("connect " + client.finishConnect());
            key.interestOps(SelectionKey.OP_WRITE);
          } else if (key.isWritable()) {
            events.add("write");
            client.write(ascii("selecteur"));
            key.interestOps(0);
          } else if (key.isReadable()) {
            ByteBuffer in = ByteBuffer.allocate(64);
            ((SocketChannel) key.channel()).read(in);
            events.add("read");
            received = text(in);
          }
        }
      }
      if (peer != null) peer.close();
      java.util.Collections.sort(events);
      return events + ", reçu " + received;
    }
  }

  // A thread blocked in SocketChannel.read; another closes the channel or interrupts the thread.
  static String blockedRead(boolean interrupt) throws Exception {
    try (ServerSocketChannel server = ServerSocketChannel.open()) {
      server.bind(new InetSocketAddress(LOOPBACK, 0));
      SocketChannel client = SocketChannel.open(server.getLocalAddress());
      try (SocketChannel peer = server.accept()) {
        String[] seen = new String[1];
        Thread reader = new Thread(() -> {
          try {
            int n = client.read(ByteBuffer.allocate(8));
            seen[0] = "lu " + n;
          } catch (IOException e) {
            seen[0] = e.getClass().getSimpleName();
          }
        });
        reader.start();
        awaitBlocked(reader);
        if (interrupt) reader.interrupt();
        else client.close();
        reader.join(5000);
        return seen[0] + ", fil terminé " + !reader.isAlive() + ", canal ouvert " + client.isOpen();
      } finally {
        client.close();
      }
    }
  }
}
