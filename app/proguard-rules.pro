# Parallax release rules.
#
# Most of what the app needs is covered by the consumer rules each module ships
# (see */consumer-rules.pro) and by the rules bundled with AndroidX, Hilt and
# kotlinx.serialization. What is left here is the app-level surface: things
# reached by name from outside Kotlin's own call graph, which R8 cannot see.

# Keep line numbers so a crash report from a release build is readable, while
# still obfuscating names.
-keepattributes SourceFile,LineNumberTable
-renamesourcefileattribute SourceFile

# Native methods are resolved by name at runtime. The engine's bridge keeps its
# own rules in engine/native/consumer-rules.pro; this covers any other class
# that gains a native method later.
-keepclasseswithmembernames,includedescriptorclasses class * {
    native <methods>;
}

# Filament's JNI layer looks up these classes and fields by name from C++.
-keep class com.google.android.filament.** { *; }
-keep class com.google.android.filament.utils.** { *; }
-dontwarn com.google.android.filament.**

# ARCore loads its implementation reflectively and warns about optional
# dependencies it does not ship.
-keep class com.google.ar.core.** { *; }
-dontwarn com.google.ar.core.**

# LiteRT delegates are instantiated by reflection when the device supports them.
-keep class org.tensorflow.lite.** { *; }
-keep class com.google.ai.edge.litert.** { *; }
-dontwarn org.tensorflow.lite.**
-dontwarn com.google.ai.edge.litert.**

# Enum valueOf is used by the DataStore settings layer to read stored names.
-keepclassmembers enum * {
    public static **[] values();
    public static ** valueOf(java.lang.String);
}

# Compose and Kotlin reflection metadata that R8's own rules do not already cover.
-dontwarn org.jetbrains.annotations.**
