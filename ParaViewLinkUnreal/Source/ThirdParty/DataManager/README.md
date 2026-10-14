# DataManager client library

Prebuilt from the DataManager repository (`include/dm/dm.h`, C API). Rebuild
and copy here when the DataManager changes:

| Path | Built with |
|---|---|
| `include/dm/dm.h` | copied from DataManager `include/dm/dm.h` |
| `lib/Win64/dmcore.dll`, `dmcore.lib` | MSVC, `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`, Release (shared) |
| `lib/Android/arm64-v8a/libdmcore.so` | NDK r27c, `ANDROID_PLATFORM=android-29`, `ANDROID_STL=c++_static`, stripped |
| `lib/Mac/libdmcore.dylib` | universal x86_64 + arm64, macOS 11 |

These are the same binaries as the Unity package
(`unity/com.tacc.datamanager/Plugins`).
