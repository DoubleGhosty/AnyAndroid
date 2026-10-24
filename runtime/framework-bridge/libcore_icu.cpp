// The ICU natives of libcore and of the i18n module (android-17.0.0_r1):
// java.lang.Character (ojluni Character.cpp), libcore.icu.ICU
// (libcore_icu_ICU.cpp) and com.android.icu.* (external/icu
// android_icu4j/libcore_bridge/src/native). Android answers them with ICU4C;
// AnyAndroid has no ICU4C, so each one calls its ICU4J counterpart in the
// same i18n module (android.icu.*), which reads the same ICU data of the
// image (one ICU release for both).
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "common/clock.h"
#include "framework-bridge/internal.h"

namespace aa::bridge {

namespace {

using rt::Handle;
using rt::kNull;
using rt::NativeArgs;
using rt::Runtime;
using S = rt::ApiStatus;

constexpr const char* kUCharacter = "Landroid/icu/lang/UCharacter;";
constexpr const char* kULocale = "Landroid/icu/util/ULocale;";

Handle call(Runtime& rt, const char* cls, const char* name, const char* sig, std::vector<uint32_t> args = {}) {
  return Handle(rt.call_static(cls, name, sig, std::move(args)));
}

// ScopedIcuLocale: icu::Locale::forLanguageTag.
Handle locale_for_tag(Runtime& rt, Handle tag) {
  if (tag == kNull) rt.throw_npe("javaLanguageTag == null");
  return call(rt, kULocale, "forLanguageTag", "(Ljava/lang/String;)Landroid/icu/util/ULocale;", {tag});
}

// ScopedIcuULoc: uloc_getName of a locale name.
Handle locale_name(Runtime& rt, Handle name) {
  if (name == kNull) rt.throw_npe("javaLocaleName == null");
  return call(rt, kULocale, "getName", "(Ljava/lang/String;)Ljava/lang/String;", {name});
}

// u_versionToString: the fields up to the last non-zero one, at least two.
std::string version_string(Runtime& rt, Handle version) {
  int32_t fields[4];
  const char* getters[4] = {"getMajor", "getMinor", "getMilli", "getMicro"};
  for (int i = 0; i < 4; ++i) fields[i] = int32_t(rt.call_virtual(version, getters[i], "()I"));
  int count = 4;
  while (count > 0 && fields[count - 1] == 0) --count;
  if (count <= 1) count = 2;
  std::string s;
  for (int i = 0; i < count; ++i) s += (i ? "." : "") + std::to_string(fields[i]);
  return s;
}

void register_character(rt::NativeRegistry& r) {
  const char* ch = "Ljava/lang/Character;";
  // u_islower, u_isupper, u_istitle, u_isdigit, u_isalpha, u_isalnum,
  // u_isIDStart, u_isIDPart, u_isWhitespace, u_isMirrored, u_isdefined,
  // u_isIDIgnorable and u_isJavaSpaceChar: UCharacter's methods of the same
  // definitions.
  static const std::pair<const char*, const char*> kPredicates[] = {
      {"isLowerCaseImpl", "isLowerCase"},
      {"isUpperCaseImpl", "isUpperCase"},
      {"isTitleCaseImpl", "isTitleCase"},
      {"isDigitImpl", "isDigit"},
      {"isLetterImpl", "isLetter"},
      {"isLetterOrDigitImpl", "isLetterOrDigit"},
      {"isUnicodeIdentifierStartImpl", "isUnicodeIdentifierStart"},
      {"isUnicodeIdentifierPartImpl", "isUnicodeIdentifierPart"},
      {"isWhitespaceImpl", "isWhitespace"},
      {"isMirroredImpl", "isMirrored"},
      {"isDefinedImpl", "isDefined"},
      {"isIdentifierIgnorableImpl", "isIdentifierIgnorable"},
      {"isSpaceCharImpl", "isSpaceChar"},
  };
  for (const auto& [native, icu] : kPredicates) {
    std::string method = icu;
    r.bind(ch, native, "(I)Z", [method](Runtime& rt, NativeArgs& a) -> uint64_t {
      return rt::ret_bool(rt.call_static(kUCharacter, method, "(I)Z", {a.raw(0)}) != 0);
    });
  }
  // u_hasBinaryProperty(UCHAR_ALPHABETIC = 0, UCHAR_IDEOGRAPHIC = 17).
  for (const auto& [native, property] : {std::pair{"isAlphabeticImpl", 0}, std::pair{"isIdeographicImpl", 17}}) {
    uint32_t p = uint32_t(property);
    r.bind(ch, native, "(I)Z", [p](Runtime& rt, NativeArgs& a) -> uint64_t {
      return rt::ret_bool(rt.call_static(kUCharacter, "hasBinaryProperty", "(II)Z", {a.raw(0), p}) != 0);
    });
  }
  // u_charType and u_charDirection: ICU values, which libcore maps to Java's.
  r.bind(ch, "getTypeImpl", "(I)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.call_static(kUCharacter, "getType", "(I)I", {a.raw(0)});
  });
  r.bind(ch, "getDirectionalityImpl", "(I)B", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt::ret_int(int8_t(rt.call_static(kUCharacter, "getDirection", "(I)I", {a.raw(0)})));
  });
  // u_tolower, u_toupper, u_totitle: simple case mappings.
  for (const auto& [native, icu] : {std::pair{"toLowerCaseImpl", "toLowerCase"}, std::pair{"toUpperCaseImpl", "toUpperCase"},
                                    std::pair{"toTitleCaseImpl", "toTitleCase"}}) {
    std::string method = icu;
    r.bind(ch, native, "(I)I", [method](Runtime& rt, NativeArgs& a) -> uint64_t {
      return rt.call_static(kUCharacter, method, "(I)I", {a.raw(0)});
    });
  }
  r.bind(ch, "digitImpl", "(II)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    return rt.call_static(kUCharacter, "digit", "(II)I", {a.raw(0), a.raw(1)});
  });
  // u_getNumericValue: -1 without a value, -2 for a negative or fractional one.
  r.bind(ch, "getNumericValueImpl", "(I)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    uint64_t bits = rt.call_static(kUCharacter, "getUnicodeNumericValue", "(I)D", {a.raw(0)});
    double v;
    std::memcpy(&v, &bits, 8);
    constexpr double kNoNumericValue = -123456789;  // U_NO_NUMERIC_VALUE
    if (v == kNoNumericValue) return rt::ret_int(-1);
    if (v < 0 || std::floor(v + 0.5) != v) return rt::ret_int(-2);
    return rt::ret_int(int32_t(v));
  });
  // u_charName: modern names, the extended ones for control characters (whose
  // only names are their Unicode 1.0 names, "NULL" rather than "BASIC LATIN 0").
  r.bind(ch, "getNameImpl", "(I)Ljava/lang/String;", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    int32_t c = a.i32(0);
    bool control = c <= 0x1f || (c >= 0x7f && c <= 0x9f);
    Handle name = call(rt, kUCharacter, control ? "getExtendedName" : "getName", "(I)Ljava/lang/String;", {a.raw(0)});
    return name != kNull && rt.string_utf8(name).empty() ? kNull : name;
  });
  // u_charFromName(U_EXTENDED_CHAR_NAME): -1 when no character has the name.
  r.bind(ch, "codePointOfImpl", "(Ljava/lang/String;)I", [](Runtime& rt, NativeArgs& a) -> uint64_t {
    if (a.h(0) == kNull) rt.throw_npe({});
    return rt.call_static(kUCharacter, "getCharFromExtendedName", "(Ljava/lang/String;)I", {a.h(0)});
  });
}

void register_icu(rt::NativeRegistry& r) {
  // Versions: those of the ICU release in the i18n module (u_getVersion,
  // u_getUnicodeVersion, ulocdata_getCLDRVersion), as u_versionToString writes them.
  auto icu_version = [](Runtime& rt, NativeArgs&) -> uint64_t {
    rt::Class* vi = rt.find_class("Landroid/icu/util/VersionInfo;");
    rt.ensure_initialized(vi);
    rt::Field* f = find_field(vi, "ICU_VERSION", "Landroid/icu/util/VersionInfo;");
    return rt.new_string_utf8(version_string(rt, Handle(vi->statics.at(f->slot))));
  };
  auto unicode_version = [](Runtime& rt, NativeArgs&) -> uint64_t {
    return rt.new_string_utf8(
        version_string(rt, call(rt, kUCharacter, "getUnicodeVersion", "()Landroid/icu/util/VersionInfo;")));
  };
  auto cldr_version = [](Runtime& rt, NativeArgs&) -> uint64_t {
    return rt.new_string_utf8(version_string(
        rt, call(rt, "Landroid/icu/util/LocaleData;", "getCLDRVersion", "()Landroid/icu/util/VersionInfo;")));
  };
  const char* meta = "Lcom/android/icu/util/Icu4cMetadata;";
  r.bind(meta, "getIcuVersion", "()Ljava/lang/String;", icu_version);
  r.bind(meta, "getUnicodeVersion", "()Ljava/lang/String;", unicode_version);
  r.bind(meta, "getCldrVersion", "()Ljava/lang/String;", cldr_version);
  // libcore.icu.ICU's versions fill System's android.icu.*.version properties
  // in System.<clinit>, before ICU4J can run (it reads its data through
  // java.io.File): they are those of the image's ICU release, which e2e
  // host-libcore compares with ICU4J's (Icu4cMetadata above).
  auto fixed = [](const char* v) {
    std::string s = v;
    return [s](Runtime& rt, NativeArgs&) -> uint64_t { return rt.new_string_utf8(s); };
  };
  r.bind("Llibcore/icu/ICU;", "getIcuVersion", "()Ljava/lang/String;", fixed("78.3"), S::kPartial,
         "version de l'ICU d'Android 17, vérifiée contre ICU4J");
  r.bind("Llibcore/icu/ICU;", "getUnicodeVersion", "()Ljava/lang/String;", fixed("17.0"), S::kPartial,
         "version de l'ICU d'Android 17, vérifiée contre ICU4J");
  r.bind("Llibcore/icu/ICU;", "getCldrVersion", "()Ljava/lang/String;", fixed("48.0"), S::kPartial,
         "version de l'ICU d'Android 17, vérifiée contre ICU4J");
  // icu::TimeZone::getTZDataVersion.
  r.bind("Lcom/android/icu/util/Icu4cMetadata;", "getTzdbVersion", "()Ljava/lang/String;",
         [](Runtime& rt, NativeArgs&) -> uint64_t {
           return call(rt, "Landroid/icu/util/TimeZone;", "getTZDataVersion", "()Ljava/lang/String;");
         });

  const char* icu = "Llibcore/icu/ICU;";
  // uloc_getScript, uloc_getISO3Country, uloc_getISO3Language of the
  // uloc_getName of the argument.
  for (const auto& [native, method] : {std::pair{"getScript", "getScript"}, std::pair{"getISO3Country", "getISO3Country"},
                                       std::pair{"getISO3Language", "getISO3Language"}}) {
    std::string m = method;
    r.bind(icu, native, "(Ljava/lang/String;)Ljava/lang/String;", [m](Runtime& rt, NativeArgs& a) -> uint64_t {
      Handle name = locale_name(rt, a.h(0));
      return call(rt, kULocale, m.c_str(), "(Ljava/lang/String;)Ljava/lang/String;", {name});
    });
  }
  r.bind(icu, "getISOCountriesNative", "()[Ljava/lang/String;", [](Runtime& rt, NativeArgs&) -> uint64_t {
    return call(rt, kULocale, "getISOCountries", "()[Ljava/lang/String;");
  });
  r.bind(icu, "getISOLanguagesNative", "()[Ljava/lang/String;", [](Runtime& rt, NativeArgs&) -> uint64_t {
    return call(rt, kULocale, "getISOLanguages", "()[Ljava/lang/String;");
  });
  // uloc_getAvailable: the ICU names ("zh_Hant_TW") of the installed locales.
  r.bind(icu, "getAvailableLocalesNative", "()[Ljava/lang/String;", [](Runtime& rt, NativeArgs&) -> uint64_t {
    Handle locales = call(rt, kULocale, "getAvailableLocales", "()[Landroid/icu/util/ULocale;");
    rt::GlobalRef keep(rt, locales);
    uint32_t n = rt.array(locales, "ULocale[]").length;
    Handle names = rt.new_array("[Ljava/lang/String;", int32_t(n));
    rt::GlobalRef keep_names(rt, names);
    for (uint32_t i = 0; i < n; ++i) {
      Handle loc;
      std::memcpy(&loc, &rt.array(locales, "ULocale[]").data[size_t(i) * 4], 4);
      Handle name = Handle(rt.call_virtual(loc, "getName", "()Ljava/lang/String;"));
      std::memcpy(&rt.array(names, "String[]").data[size_t(i) * 4], &name, 4);
    }
    return names;
  });

  // LocaleNative: icu::Locale::getDisplay* of the target locale, in the display locale.
  const char* ln = "Lcom/android/icu/util/LocaleNative;";
  for (const auto& [native, method] :
       {std::pair{"getDisplayCountryNative", "getDisplayCountry"}, std::pair{"getDisplayLanguageNative", "getDisplayLanguage"},
        std::pair{"getDisplayScriptNative", "getDisplayScript"}, std::pair{"getDisplayVariantNative", "getDisplayVariant"}}) {
    std::string m = method;
    r.bind(ln, native, "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", [m](Runtime& rt, NativeArgs& a) -> uint64_t {
      Handle display = locale_for_tag(rt, a.h(1));
      rt::GlobalRef keep(rt, display);
      Handle target = locale_for_tag(rt, a.h(0));
      return rt.call_virtual(target, m, "(Landroid/icu/util/ULocale;)Ljava/lang/String;", {display});
    });
  }

  // CaseMapperNative: UnicodeString::toLower/toUpper with the locale's rules
  // (full case mapping); the argument itself when nothing changes.
  const char* cm = "Lcom/android/icu/util/CaseMapperNative;";
  for (const auto& [native, method] : {std::pair{"toLowerCase", "toLowerCase"}, std::pair{"toUpperCase", "toUpperCase"}}) {
    std::string m = method;
    r.bind(cm, native, "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", [m](Runtime& rt, NativeArgs& a) -> uint64_t {
      if (a.h(0) == kNull) rt.throw_npe({});
      Handle locale = locale_for_tag(rt, a.h(1));
      Handle mapped = call(rt, kUCharacter, m.c_str(), "(Landroid/icu/util/ULocale;Ljava/lang/String;)Ljava/lang/String;",
                           {locale, a.h(0)});
      return rt.string_value(mapped) == rt.string_value(a.h(0)) ? a.h(0) : mapped;
    });
  }

  // TimeZoneNamesNative.fillZoneStringsNative: for each row [id, ...], the long
  // and short standard and daylight names at the current time, the ID
  // canonicalized by ICU; names starting with "GMT" are left null. As in the
  // C++ code, once an ID fails to canonicalize the status stays failed and the
  // later rows use their IDs as given.
  r.bind("Lcom/android/icu/text/TimeZoneNamesNative;", "fillZoneStringsNative", "(Ljava/lang/String;[[Ljava/lang/String;)V",
         [](Runtime& rt, NativeArgs& a) -> uint64_t {
           Handle locale = locale_for_tag(rt, a.h(0));
           rt::GlobalRef keep_locale(rt, locale);
           Handle names = call(rt, "Landroid/icu/text/TimeZoneNames;", "getInstance",
                               "(Landroid/icu/util/ULocale;)Landroid/icu/text/TimeZoneNames;", {locale});
           rt::GlobalRef keep_names(rt, names);
           rt::Class* type_class = rt.find_class("Landroid/icu/text/TimeZoneNames$NameType;");
           rt.ensure_initialized(type_class);
           const char* kTypes[4] = {"LONG_STANDARD", "SHORT_STANDARD", "LONG_DAYLIGHT", "SHORT_DAYLIGHT"};
           Handle types[4];
           for (int i = 0; i < 4; ++i) {
             types[i] = Handle(type_class->statics.at(find_field(type_class, kTypes[i], "Landroid/icu/text/TimeZoneNames$NameType;")->slot));
           }
           const int64_t now = clock::unix_millis();
           Handle result = a.h(1);
           if (result == kNull) rt.throw_npe({});
           bool failed = false;
           uint32_t rows = rt.array(result, "String[][]").length;
           for (uint32_t i = 0; i < rows; ++i) {
             Handle row;
             std::memcpy(&row, &rt.array(result, "String[][]").data[size_t(i) * 4], 4);
             Handle id;
             std::memcpy(&id, &rt.array(row, "String[]").data[0], 4);
             if (id == kNull) rt.throw_npe({});
             Handle lookup = id;
             if (!failed) {
               Handle canonical = call(rt, "Landroid/icu/util/TimeZone;", "getCanonicalID", "(Ljava/lang/String;)Ljava/lang/String;", {id});
               if (canonical == kNull) {
                 failed = true;
               } else {
                 lookup = canonical;
               }
             }
             rt::GlobalRef keep_lookup(rt, lookup);
             for (int k = 0; k < 4; ++k) {
               Handle name = Handle(rt.call_virtual(names, "getDisplayName",
                                                    "(Ljava/lang/String;Landroid/icu/text/TimeZoneNames$NameType;J)Ljava/lang/String;",
                                                    {lookup, types[k], uint32_t(now), uint32_t(uint64_t(now) >> 32)}));
               if (name == kNull || rt.string_utf8(name).rfind("GMT", 0) == 0) continue;
               std::memcpy(&rt.array(row, "String[]").data[size_t(k + 1) * 4], &name, 4);
             }
           }
           return 0;
         });
  // UResourceBundleNative.cacheTimeZoneBundles warms ICU4C's resource cache;
  // without ICU4C there is nothing to warm.
  r.bind("Lcom/android/icu/util/UResourceBundleNative;", "cacheTimeZoneBundles", "()V",
         [](Runtime&, NativeArgs&) -> uint64_t { return 0; }, S::kPartial, "pas d'ICU4C : aucun cache à préremplir");
}

}  // namespace

void register_libcore_icu(rt::NativeRegistry& r) {
  register_character(r);
  register_icu(r);
}

}  // namespace aa::bridge
