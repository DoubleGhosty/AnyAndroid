import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Writes aa/host/IndySites.class: invokedynamic call sites with custom bootstrap methods, and ldc
 * of MethodHandle and MethodType constants, which javac never writes from Java source. D8 turns
 * them into invoke-custom, const-method-handle and const-method-type (min-api 28 and up), as in an
 * application. The bootstrap methods and targets are Java code (aa.host.Indy); LibcoreProbe calls
 * these methods on AOSP's real libcore.
 *
 * <p>Usage: java IndySitesGen OUTPUT_DIR
 */
public final class IndySitesGen {
  private static final String INDY = "aa/host/Indy";
  private static final String LOOKUP = "Ljava/lang/invoke/MethodHandles$Lookup;";
  private static final String CALL_SITE = "Ljava/lang/invoke/CallSite;";
  // Method handle kinds of the class file format (JVMS 4.4.8).
  private static final int REF_GET_STATIC = 2;
  private static final int REF_INVOKE_VIRTUAL = 5;
  private static final int REF_INVOKE_STATIC = 6;
  private static final int REF_NEW_INVOKE_SPECIAL = 8;
  private static final int REF_INVOKE_INTERFACE = 9;

  private interface Writer {
    void write(DataOutputStream out) throws IOException;
  }

  private final ByteArrayOutputStream poolBytes = new ByteArrayOutputStream();
  private final DataOutputStream pool = new DataOutputStream(poolBytes);
  private final Map<String, Integer> entries = new HashMap<>();
  private int poolCount = 1;
  private final List<int[]> bootstraps = new ArrayList<>();
  private final ByteArrayOutputStream methodBytes = new ByteArrayOutputStream();
  private final DataOutputStream methods = new DataOutputStream(methodBytes);
  private int methodCount;

  private int entry(String key, int slots, Writer writer) throws IOException {
    Integer index = entries.get(key);
    if (index != null) return index;
    writer.write(pool);
    int at = poolCount;
    poolCount += slots;
    entries.put(key, at);
    return at;
  }

  private int utf8(String s) throws IOException {
    return entry("U" + s, 1, out -> {
      out.writeByte(1);
      out.writeUTF(s);
    });
  }

  private int classRef(String name) throws IOException {
    int n = utf8(name);
    return entry("C" + name, 1, out -> {
      out.writeByte(7);
      out.writeShort(n);
    });
  }

  private int string(String s) throws IOException {
    int n = utf8(s);
    return entry("S" + s, 1, out -> {
      out.writeByte(8);
      out.writeShort(n);
    });
  }

  private int integer(int v) throws IOException {
    return entry("I" + v, 1, out -> {
      out.writeByte(3);
      out.writeInt(v);
    });
  }

  private int longValue(long v) throws IOException {
    return entry("J" + v, 2, out -> {
      out.writeByte(5);
      out.writeLong(v);
    });
  }

  private int floatValue(float v) throws IOException {
    return entry("F" + v, 1, out -> {
      out.writeByte(4);
      out.writeFloat(v);
    });
  }

  private int doubleValue(double v) throws IOException {
    return entry("D" + v, 2, out -> {
      out.writeByte(6);
      out.writeDouble(v);
    });
  }

  private int nameAndType(String name, String descriptor) throws IOException {
    int n = utf8(name);
    int d = utf8(descriptor);
    return entry("N" + name + ":" + descriptor, 1, out -> {
      out.writeByte(12);
      out.writeShort(n);
      out.writeShort(d);
    });
  }

  private int memberRef(int tag, String owner, String name, String descriptor) throws IOException {
    int c = classRef(owner);
    int nt = nameAndType(name, descriptor);
    return entry("R" + tag + owner + "." + name + ":" + descriptor, 1, out -> {
      out.writeByte(tag);
      out.writeShort(c);
      out.writeShort(nt);
    });
  }

  private int methodHandle(int kind, int tag, String owner, String name, String descriptor) throws IOException {
    int ref = memberRef(tag, owner, name, descriptor);
    return entry("H" + kind + ":" + ref, 1, out -> {
      out.writeByte(15);
      out.writeByte(kind);
      out.writeShort(ref);
    });
  }

  private int staticMethodHandle(String owner, String name, String descriptor) throws IOException {
    return methodHandle(REF_INVOKE_STATIC, 10, owner, name, descriptor);
  }

  private int methodType(String descriptor) throws IOException {
    int d = utf8(descriptor);
    return entry("T" + descriptor, 1, out -> {
      out.writeByte(16);
      out.writeShort(d);
    });
  }

  /** An invokedynamic constant whose bootstrap method is Indy.name with these static arguments. */
  private int callSite(String name, String descriptor, String bootstrap, String bootstrapDescriptor, int... args)
      throws IOException {
    int[] bsm = new int[args.length + 1];
    bsm[0] = staticMethodHandle(INDY, bootstrap, bootstrapDescriptor);
    System.arraycopy(args, 0, bsm, 1, args.length);
    int index = bootstraps.size();
    bootstraps.add(bsm);
    int nt = nameAndType(name, descriptor);
    return entry("Y" + index, 1, out -> {
      out.writeByte(18);
      out.writeShort(index);
      out.writeShort(nt);
    });
  }

  /** A public static method whose body is `code` (no branches, so no stack map). */
  private void method(String name, String descriptor, int maxLocals, byte[] code) throws IOException {
    int n = utf8(name);
    int d = utf8(descriptor);
    int codeName = utf8("Code");
    methods.writeShort(0x0009);  // public static
    methods.writeShort(n);
    methods.writeShort(d);
    methods.writeShort(1);
    methods.writeShort(codeName);
    methods.writeInt(12 + code.length);
    methods.writeShort(2);  // max_stack
    methods.writeShort(maxLocals);
    methods.writeInt(code.length);
    methods.write(code);
    methods.writeShort(0);  // exception table
    methods.writeShort(0);  // attributes
    ++methodCount;
  }

  private static byte[] invokeDynamic(int index, int load, int ret) {
    byte[] call = {(byte) 0xba, (byte) (index >> 8), (byte) index, 0, 0, (byte) ret};
    if (load < 0) return call;
    byte[] code = new byte[call.length + 1];
    code[0] = (byte) load;
    System.arraycopy(call, 0, code, 1, call.length);
    return code;
  }

  private static byte[] loadConstant(int index) {
    return new byte[] {0x13, (byte) (index >> 8), (byte) index, (byte) 0xb0};  // ldc_w, areturn
  }

  private byte[] build() throws IOException {
    final int aload = -1;
    final int iload0 = 0x1a;
    final int areturn = 0xb0;
    final int ireturn = 0xac;
    String describe = "(" + LOOKUP + "Ljava/lang/String;Ljava/lang/invoke/MethodType;IJFDLjava/lang/String;"
        + "Ljava/lang/Class;Ljava/lang/invoke/MethodHandle;Ljava/lang/invoke/MethodType;)" + CALL_SITE;
    method("describe", "()Ljava/lang/String;", 0, invokeDynamic(callSite("describe", "()Ljava/lang/String;",
        "describe", describe, integer(42), longValue(7), floatValue(1.5f), doubleValue(2.25), string("ancre"),
        classRef("java/lang/Integer"), staticMethodHandle(INDY, "twice", "(I)I"), methodType("(I)I")),
        aload, areturn));
    String constant = "(" + LOOKUP + "Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;)"
        + CALL_SITE;
    method("twice", "(I)I", 1, invokeDynamic(callSite("twice", "(I)I", "constant", constant,
        staticMethodHandle(INDY, "twice", "(I)I")), iload0, ireturn));
    String varargs = "(" + LOOKUP + "Ljava/lang/String;Ljava/lang/invoke/MethodType;[Ljava/lang/String;)" + CALL_SITE;
    method("joined", "()Ljava/lang/String;", 0, invokeDynamic(callSite("joined", "()Ljava/lang/String;", "varargs",
        varargs, string("a"), string("b"), string("c")), aload, areturn));
    String nothing = "(" + LOOKUP + "Ljava/lang/String;Ljava/lang/invoke/MethodType;)" + CALL_SITE;
    method("broken", "()Ljava/lang/Object;", 0, invokeDynamic(callSite("broken", "()Ljava/lang/Object;", "nothing",
        nothing), aload, areturn));
    method("handle", "()Ljava/lang/invoke/MethodHandle;", 0, loadConstant(staticMethodHandle(INDY, "twice", "(I)I")));
    method("type", "()Ljava/lang/invoke/MethodType;", 0, loadConstant(methodType("(ILjava/lang/String;)J")));
    method("getter", "()Ljava/lang/invoke/MethodHandle;", 0,
        loadConstant(methodHandle(REF_GET_STATIC, 9, INDY, "counter", "I")));
    method("virtualHandle", "()Ljava/lang/invoke/MethodHandle;", 0,
        loadConstant(methodHandle(REF_INVOKE_VIRTUAL, 10, "java/lang/String", "length", "()I")));
    method("interfaceHandle", "()Ljava/lang/invoke/MethodHandle;", 0,
        loadConstant(methodHandle(REF_INVOKE_INTERFACE, 11, "java/lang/CharSequence", "length", "()I")));
    method("constructorHandle", "()Ljava/lang/invoke/MethodHandle;", 0, loadConstant(
        methodHandle(REF_NEW_INVOKE_SPECIAL, 10, "java/lang/StringBuilder", "<init>", "(Ljava/lang/String;)V")));

    int thisClass = classRef("aa/host/IndySites");
    int superClass = classRef("java/lang/Object");
    int bootstrapName = utf8("BootstrapMethods");
    ByteArrayOutputStream bytes = new ByteArrayOutputStream();
    DataOutputStream out = new DataOutputStream(bytes);
    out.writeInt(0xCAFEBABE);
    out.writeShort(0);
    out.writeShort(52);  // Java 8: no stack map for straight-line code
    out.writeShort(poolCount);
    out.write(poolBytes.toByteArray());
    out.writeShort(0x0031);  // public final super
    out.writeShort(thisClass);
    out.writeShort(superClass);
    out.writeShort(0);  // interfaces
    out.writeShort(0);  // fields
    out.writeShort(methodCount);
    out.write(methodBytes.toByteArray());
    out.writeShort(1);  // attributes: BootstrapMethods
    out.writeShort(bootstrapName);
    int length = 2;
    for (int[] b : bootstraps) length += 2 * (b.length + 1);
    out.writeInt(length);
    out.writeShort(bootstraps.size());
    for (int[] b : bootstraps) {
      out.writeShort(b[0]);
      out.writeShort(b.length - 1);
      for (int i = 1; i < b.length; ++i) out.writeShort(b[i]);
    }
    return bytes.toByteArray();
  }

  public static void main(String[] args) throws IOException {
    Path file = Path.of(args[0], "aa", "host", "IndySites.class");
    Files.createDirectories(file.getParent());
    Files.write(file, new IndySitesGen().build());
  }
}
