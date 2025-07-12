# Add project specific ProGuard rules here.
# You can control the set of applied configuration files using the
# proguardFiles setting in build.gradle.
#
# For more details, see
#   http://developer.android.com/guide/developing/tools/proguard.html

# If your project uses WebView with JS, uncomment the following
# and specify the fully qualified class name to the JavaScript interface
# class:
#-keepclassmembers class fqcn.of.javascript.interface.for.webview {
#   public *;
#}

# Uncomment this to preserve the line number information for
# debugging stack traces.
# -keepattributes SourceFile,LineNumberTable

# If you keep the line number information, uncomment this to
# hide the original source file name.
-renamesourcefileattribute SourceFile

-dontusemixedcaseclassnames
-dontskipnonpubliclibraryclasses
-verbose

# If you want to enable optimization, you should include the comenta o proguard pra ve
# following:
 -optimizations !code/simplification/arithmetic,!code/simplification/cast,!field/*,!class/merging/*
 -optimizationpasses 5
 -allowaccessmodification

# Specify the package name to apply string encryption
 -keeppackagenames com.revc.game

-keepattributes Signature
-keepattributes *Annotation*

 -dontwarn com.revc.game.MainActivity*
 -keep class com.revc.game.MainActivity { *; }

 -dontwarn androidx.**
 -keep class androidx.** { *; }

 -dontwarn org.ini4j.**
 -keep class org.ini4j.** { *; }

 -repackageclasses 'a'

 -keepattributes Exceptions, InnerClasses, Signature, Deprecated, SourceFile,
 LineNumberTable, *Annotation*, EnclosingMethod
 -dontwarn android.webkit.JavascriptInterface

 -dontwarn org.jetbrains.annotations.**

-assumenosideeffects class android.util.Log {
    public static *** d(...);
    public static *** v(...);
    public static *** w(...);
    public static *** i(...);
}
