# Consumer ProGuard rules for this module.
#
# Rules here are applied to any app that depends on this library, so only
# genuinely necessary keeps belong in it: everything kept here is kept in every
# consumer's release build whether they use it or not.

# The JNI bridge resolves these by name from C++, so neither the class nor the
# native method signatures may be renamed.
-keep class uk.co.jacoblevy.parallax.engine.nativebridge.** { *; }
-keepclasseswithmembernames class * {
    native <methods>;
}
