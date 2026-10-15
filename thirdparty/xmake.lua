includes("libsrtp", "openfec", "libyuv", "aom", "svt-av1", "openh264", "dav1d", "glib", "gupnp", "libnice", "websocketpp", "libdatachannel")
if is_plat("iphoneos", "android", "harmony") then
    includes("openssl")
end
if is_plat("harmony") then
    includes("libffi")
end
