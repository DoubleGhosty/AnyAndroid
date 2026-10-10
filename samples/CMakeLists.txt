# Demo APKs, rebuilt from their text sources (AndroidManifest.xml + smali) by
# apkgen at build time: no Android SDK, no binary blobs in the repository.

set(AA_SAMPLES_DIR ${CMAKE_BINARY_DIR}/samples CACHE INTERNAL "Dossier des APK de démonstration")
set(outputs "")

function(aa_sample name)
  cmake_parse_arguments(S "" "" "EXTRA_ARGS" ${ARGN})
  set(dir ${CMAKE_CURRENT_SOURCE_DIR}/${name})
  file(GLOB_RECURSE smali CONFIGURE_DEPENDS ${dir}/smali/*.smali)
  set(out ${AA_SAMPLES_DIR}/${name}.apk)
  add_custom_command(OUTPUT ${out}
    COMMAND apkgen build --manifest ${dir}/AndroidManifest.xml --smali ${dir}/smali ${S_EXTRA_ARGS} -o ${out}
    DEPENDS apkgen ${dir}/AndroidManifest.xml ${smali}
    COMMENT "apkgen : ${name}.apk"
    VERBATIM)
  set(outputs ${outputs} ${out} PARENT_SCOPE)
endfunction()

aa_sample(hello-dex)
aa_sample(threads-demo)
aa_sample(libcore-demo)
aa_sample(framework-demo)
aa_sample(basic-activity)
aa_sample(two-screens)
aa_sample(native-lib EXTRA_ARGS
  --stub-lib lib/arm64-v8a/libnative.so:aarch64
  --stub-lib lib/x86_64/libnative.so:x86_64)

# GMS-like profile delivered as an app bundle: base + per-ABI config splits.
aa_sample(gms-profile)
set(gp ${CMAKE_CURRENT_SOURCE_DIR}/gms-profile/splits)
foreach(split x86_64 arm64_v8a)
  if(split STREQUAL "x86_64")
    set(lib lib/x86_64/libgmscore.so:x86_64)
  else()
    set(lib lib/arm64-v8a/libgmscore.so:aarch64)
  endif()
  set(out ${AA_SAMPLES_DIR}/split_config.${split}.apk)
  add_custom_command(OUTPUT ${out}
    COMMAND apkgen build --manifest ${gp}/config.${split}.xml --stub-lib ${lib} -o ${out}
    DEPENDS apkgen ${gp}/config.${split}.xml
    COMMENT "apkgen : split_config.${split}.apk"
    VERBATIM)
  list(APPEND outputs ${out})
endforeach()
add_custom_command(OUTPUT ${AA_SAMPLES_DIR}/gms-profile.apks
  COMMAND apkgen bundle -o ${AA_SAMPLES_DIR}/gms-profile.apks ${AA_SAMPLES_DIR}/gms-profile.apk
          ${AA_SAMPLES_DIR}/split_config.x86_64.apk ${AA_SAMPLES_DIR}/split_config.arm64_v8a.apk
  DEPENDS apkgen ${AA_SAMPLES_DIR}/gms-profile.apk ${AA_SAMPLES_DIR}/split_config.x86_64.apk
          ${AA_SAMPLES_DIR}/split_config.arm64_v8a.apk
  COMMENT "apkgen : gms-profile.apks"
  VERBATIM)
list(APPEND outputs ${AA_SAMPLES_DIR}/gms-profile.apks)

# Malformed archives derived from basic-activity.apk (robustness tests).
foreach(kind truncated bad-crc traversal duplicate bomb bad-manifest bad-dex)
  set(out ${AA_SAMPLES_DIR}/malformed-${kind}.apk)
  add_custom_command(OUTPUT ${out}
    COMMAND apkgen corrupt ${kind} ${AA_SAMPLES_DIR}/basic-activity.apk -o ${out}
    DEPENDS apkgen ${AA_SAMPLES_DIR}/basic-activity.apk
    COMMENT "apkgen : malformed-${kind}.apk"
    VERBATIM)
  list(APPEND outputs ${out})
endforeach()

add_custom_target(samples ALL DEPENDS ${outputs})
