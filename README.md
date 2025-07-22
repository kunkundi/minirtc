# projectx

vcpkg/buildtrees/versioning_/versions/pcre/69e232f12c4e3eab4115f0672466a6661978bea2$ vim portfile.cmake

-    URLS "https://ftp.pcre.org/pub/pcre/pcre-${PCRE_VERSION}.zip"
+    URLS "https://sourceforge.net/projects/pcre/files/pcre/${PCRE_VERSION}/pcre-${PCRE_VERSION}.zip"

linux

sudo apt-get install nvidia-cuda-toolkit
solve <cuda.h>

sudo apt-get install libxcb-randr0-dev libxcb-xtest0-dev libxcb-xinerama0-dev libxcb-shape0-dev libxcb-xkb-dev libxcb-xfixes0-dev libxv-dev libxtst-dev
solve x11

sudo apt-get -y install libasound2-dev libsndio-dev libxcb-shm0-dev
solve asound sndio xcb-shm

sudo apt-get -y install libasound2-dev libpulse-dev && rebuild
solve error dsp no such audio device

sudo apt-get install libavcodec-dev libavformat-dev libavutil-dev libavfilter-dev libavdevice-dev

sudo apt remove libssl-dev libglib2.0-dev

install:
	@echo hello world
	install -D build/linux/x86_64/release/remote_desk -t /usr/bin
	install -D config/config.ini -t /usr/bin
	install -D build/linux/x86_64/release/libprojectx.so -t /usr/lib
	install -D thirdparty/nvcodec/Lib/x64/libnvidia-encode.so.1 -t /usr/lib
	install -D thirdparty/nvcodec/Lib/x64/libnvidia-encode.so -t /usr/lib
	install -D thirdparty/nvcodec/Lib/x64/libnvcuvid.so.1 -t /usr/lib
	install -D thirdparty/nvcodec/Lib/x64/libnvcuvid.so -t /usr/lib