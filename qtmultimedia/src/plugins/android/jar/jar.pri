load(qt_build_paths)
CONFIG += java
DESTDIR = $$MODULE_BASE_OUTDIR/jar
API_VERSION = android-11

JAVACLASSPATH += $$PWD/src

JAVASOURCES += $$PWD/src/org/qtproject/qt5/android/multimedia/QtAndroidMediaPlayer.java \
               $$PWD/src/org/qtproject/qt5/android/multimedia/QtCamera.java \
               $$PWD/src/org/qtproject/qt5/android/multimedia/QtSurfaceTexture.java \
               $$PWD/src/org/qtproject/qt5/android/multimedia/QtSurfaceTextureHolder.java \
               $$PWD/src/org/qtproject/qt5/android/multimedia/QtMultimediaUtils.java \
               $$PWD/src/org/qtproject/qt5/android/multimedia/QtMediaRecorder.java

# install
target.path = $$[QT_INSTALL_PREFIX]/jar
INSTALLS += target

OTHER_FILES += $$JAVASOURCES
