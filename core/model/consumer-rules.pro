# Consumer ProGuard rules for this module.
#
# Rules here are applied to any app that depends on this library, so only
# genuinely necessary keeps belong in it: everything kept here is kept in every
# consumer's release build whether they use it or not.

# kotlinx.serialization generates a synthetic $$serializer for every @Serializable
# class and reaches it reflectively from the companion. Without these keeps, R8
# strips the serializers and every project read from the database or a bundle
# fails at runtime with a SerializationException.
-keepattributes *Annotation*, InnerClasses
-dontnote kotlinx.serialization.**

-keepclassmembers class uk.co.jacoblevy.parallax.core.model.** {
    *** Companion;
}
-keepclasseswithmembers class uk.co.jacoblevy.parallax.core.model.** {
    kotlinx.serialization.KSerializer serializer(...);
}
-keep,includedescriptorclasses class uk.co.jacoblevy.parallax.core.model.**$$serializer { *; }

# Sealed interface subclasses are resolved by name through the polymorphic
# serializer, so their names must survive.
-keepnames class uk.co.jacoblevy.parallax.core.model.**
