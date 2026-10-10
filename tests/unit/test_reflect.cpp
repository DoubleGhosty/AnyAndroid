// G1: java.lang.reflect, exercised from bytecode as real code would.

#include <memory>
#include <string>

#include "dex-engine/dex_file.h"
#include "dex-engine/runtime.h"
#include "dex_writer.h"
#include "framework-bridge/bridge.h"
#include "smali.h"
#include "test.h"

using namespace aa;

namespace {

const char* kPoint = R"SMALI(
.class public LPoint;
.super Ljava/lang/Object;
.field public x:I
.field private name:Ljava/lang/String;
.field public static count:I

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public constructor <init>(ILjava/lang/String;)V
    .registers 3
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    iput p1, p0, LPoint;->x:I
    iput-object p2, p0, LPoint;->name:Ljava/lang/String;
    return-void
.end method

.method public scaled(I)I
    .registers 3
    iget v0, p0, LPoint;->x:I
    mul-int/2addr v0, p1
    return v0
.end method

.method private secret()Ljava/lang/String;
    .registers 2
    const-string v0, "caché"
    return-object v0
.end method

.method public static add(JD)D
    .registers 6
    long-to-double v0, p0
    add-double/2addr v0, p2
    return-wide v0
.end method

.method public fail()V
    .registers 3
    new-instance v0, Ljava/lang/IllegalStateException;
    const-string v1, "raté"
    invoke-direct {v0, v1}, Ljava/lang/IllegalStateException;-><init>(Ljava/lang/String;)V
    throw v0
.end method
)SMALI";

const char* kReflectTest = R"SMALI(
.class public LReflectTest;
.super Ljava/lang/Object;

.method public static run()Ljava/lang/String;
    .registers 12
    new-instance v0, Ljava/lang/StringBuilder;
    invoke-direct {v0}, Ljava/lang/StringBuilder;-><init>()V
    const-string v1, "Point"
    invoke-static {v1}, Ljava/lang/Class;->forName(Ljava/lang/String;)Ljava/lang/Class;
    move-result-object v1

    const/4 v2, 0x2
    new-array v2, v2, [Ljava/lang/Class;
    sget-object v3, Ljava/lang/Integer;->TYPE:Ljava/lang/Class;
    const/4 v4, 0x0
    aput-object v3, v2, v4
    const-class v3, Ljava/lang/String;
    const/4 v4, 0x1
    aput-object v3, v2, v4
    invoke-virtual {v1, v2}, Ljava/lang/Class;->getConstructor([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;
    move-result-object v2
    const/4 v3, 0x2
    new-array v3, v3, [Ljava/lang/Object;
    const/4 v4, 0x3
    invoke-static {v4}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v4
    const/4 v5, 0x0
    aput-object v4, v3, v5
    const-string v4, "A"
    const/4 v5, 0x1
    aput-object v4, v3, v5
    invoke-virtual {v2, v3}, Ljava/lang/reflect/Constructor;->newInstance([Ljava/lang/Object;)Ljava/lang/Object;
    move-result-object v2

    const/4 v3, 0x1
    new-array v3, v3, [Ljava/lang/Class;
    sget-object v4, Ljava/lang/Integer;->TYPE:Ljava/lang/Class;
    const/4 v5, 0x0
    aput-object v4, v3, v5
    const-string v4, "scaled"
    invoke-virtual {v1, v4, v3}, Ljava/lang/Class;->getMethod(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;
    move-result-object v3
    const/4 v4, 0x1
    new-array v4, v4, [Ljava/lang/Object;
    const/4 v5, 0x7
    invoke-static {v5}, Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;
    move-result-object v5
    const/4 v6, 0x0
    aput-object v5, v4, v6
    invoke-virtual {v3, v2, v4}, Ljava/lang/reflect/Method;->invoke(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;
    move-result-object v3
    invoke-virtual {v0, v3}, Ljava/lang/StringBuilder;->append(Ljava/lang/Object;)Ljava/lang/StringBuilder;
    const-string v3, "|"
    invoke-virtual {v0, v3}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;

    const-string v3, "x"
    invoke-virtual {v1, v3}, Ljava/lang/Class;->getField(Ljava/lang/String;)Ljava/lang/reflect/Field;
    move-result-object v3
    const/16 v4, 0xa
    invoke-virtual {v3, v2, v4}, Ljava/lang/reflect/Field;->setInt(Ljava/lang/Object;I)V
    invoke-virtual {v3, v2}, Ljava/lang/reflect/Field;->getInt(Ljava/lang/Object;)I
    move-result v3
    invoke-virtual {v0, v3}, Ljava/lang/StringBuilder;->append(I)Ljava/lang/StringBuilder;
    const-string v3, "|"
    invoke-virtual {v0, v3}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;

    const/4 v3, 0x0
    new-array v3, v3, [Ljava/lang/Class;
    const-string v4, "secret"
    invoke-virtual {v1, v4, v3}, Ljava/lang/Class;->getDeclaredMethod(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;
    move-result-object v3
    const/4 v4, 0x1
    invoke-virtual {v3, v4}, Ljava/lang/reflect/Method;->setAccessible(Z)V
    const/4 v4, 0x0
    new-array v4, v4, [Ljava/lang/Object;
    invoke-virtual {v3, v2, v4}, Ljava/lang/reflect/Method;->invoke(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;
    move-result-object v3
    invoke-virtual {v0, v3}, Ljava/lang/StringBuilder;->append(Ljava/lang/Object;)Ljava/lang/StringBuilder;
    const-string v3, "|"
    invoke-virtual {v0, v3}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;

    :try_start
    const/4 v3, 0x0
    new-array v3, v3, [Ljava/lang/Class;
    const-string v4, "fail"
    invoke-virtual {v1, v4, v3}, Ljava/lang/Class;->getMethod(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;
    move-result-object v3
    const/4 v4, 0x0
    new-array v4, v4, [Ljava/lang/Object;
    invoke-virtual {v3, v2, v4}, Ljava/lang/reflect/Method;->invoke(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;
    :try_end
    .catch Ljava/lang/reflect/InvocationTargetException; {:try_start .. :try_end} :ite
    goto :after_fail
    :ite
    move-exception v3
    invoke-virtual {v3}, Ljava/lang/Throwable;->getCause()Ljava/lang/Throwable;
    move-result-object v3
    invoke-virtual {v3}, Ljava/lang/Throwable;->getMessage()Ljava/lang/String;
    move-result-object v3
    invoke-virtual {v0, v3}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;
    :after_fail
    const-string v3, "|"
    invoke-virtual {v0, v3}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;

    const/4 v3, 0x2
    new-array v3, v3, [Ljava/lang/Class;
    sget-object v4, Ljava/lang/Long;->TYPE:Ljava/lang/Class;
    const/4 v5, 0x0
    aput-object v4, v3, v5
    sget-object v4, Ljava/lang/Double;->TYPE:Ljava/lang/Class;
    const/4 v5, 0x1
    aput-object v4, v3, v5
    const-string v4, "add"
    invoke-virtual {v1, v4, v3}, Ljava/lang/Class;->getMethod(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;
    move-result-object v3
    const/4 v4, 0x2
    new-array v4, v4, [Ljava/lang/Object;
    const-wide/16 v5, 0x2
    invoke-static {v5, v6}, Ljava/lang/Long;->valueOf(J)Ljava/lang/Long;
    move-result-object v5
    const/4 v6, 0x0
    aput-object v5, v4, v6
    const-wide/high16 v5, 0x3fe0000000000000L
    invoke-static {v5, v6}, Ljava/lang/Double;->valueOf(D)Ljava/lang/Double;
    move-result-object v5
    const/4 v6, 0x1
    aput-object v5, v4, v6
    const/4 v5, 0x0
    invoke-virtual {v3, v5, v4}, Ljava/lang/reflect/Method;->invoke(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;
    move-result-object v3
    invoke-virtual {v0, v3}, Ljava/lang/StringBuilder;->append(Ljava/lang/Object;)Ljava/lang/StringBuilder;
    const-string v3, "|"
    invoke-virtual {v0, v3}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;

    invoke-virtual {v1}, Ljava/lang/Class;->getDeclaredFields()[Ljava/lang/reflect/Field;
    move-result-object v3
    array-length v3, v3
    invoke-virtual {v0, v3}, Ljava/lang/StringBuilder;->append(I)Ljava/lang/StringBuilder;
    const-string v3, "|"
    invoke-virtual {v0, v3}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;

    :try2_start
    const-string v3, "does.not.Exist"
    invoke-static {v3}, Ljava/lang/Class;->forName(Ljava/lang/String;)Ljava/lang/Class;
    :try2_end
    .catch Ljava/lang/ClassNotFoundException; {:try2_start .. :try2_end} :cnfe
    const-string v3, "absente ?"
    goto :append_cnfe
    :cnfe
    move-exception v3
    const-string v3, "CNFE"
    :append_cnfe
    invoke-virtual {v0, v3}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;

    invoke-virtual {v0}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method
)SMALI";

}  // namespace

TEST(reflection_from_bytecode) {
  std::vector<tool::ClassDef> classes;
  for (const char* s : {kPoint, kReflectTest}) classes.push_back(tool::assemble_smali(s, "reflect.smali"));
  auto dex = dex::DexFile::open(tool::write_dex(classes), "reflect.dex");
  bridge::NullHost host;
  auto runtime = std::make_unique<rt::Runtime>();
  bridge::install(runtime->registry(), host);
  runtime->add_dex(dex.get());
  {
    rt::Runtime::Scope scope(*runtime, "test");
    std::string result;
    try {
      result = runtime->string_utf8(rt::Handle(runtime->call_static("LReflectTest;", "run", "()Ljava/lang/String;")));
    } catch (const rt::JavaThrow& t) {
      result = runtime->describe_throwable(t.exception);
    }
    CHECK_EQ(result, std::string("21|10|caché|raté|2.5|3|CNFE"));
  }
}
