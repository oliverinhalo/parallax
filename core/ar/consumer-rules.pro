# ARCore's native code reaches back into these classes by name, so R8 must not
# rename or remove them. The ARCore AAR ships its own rules for its own
# classes; this file covers nothing beyond what this module adds, which is one
# renderer with no reflection in it.
