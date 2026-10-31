// ---------------------------------------------------------------------------
// NetProxy Android SDK（AAR）
//
// 预编译 native 库需先由本仓库的 CMake 生成，再拷贝进 jniLibs：
//   cd ../../            # 仓库根目录
//   for abi in arm64-v8a armeabi-v7a; do
//     cmake --preset android-$abi && cmake --build --preset android-$abi
//     mkdir -p sdk/android/src/main/jniLibs/$abi
//     cp build/android-$abi/lib/libnetproxy.so sdk/android/src/main/jniLibs/$abi/
//     cp "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/$abi/libc++_shared.so" \
//        sdk/android/src/main/jniLibs/$abi/
//   done
//
// 产物：build/outputs/aar/netproxy-android-sdk-release.aar（本地文件交付）
// ---------------------------------------------------------------------------

plugins {
    id("com.android.library") version "8.7.3"
}

version = "0.1.0"

android {
    namespace = "com.open.netproxy"
    compileSdk = 35

    defaultConfig {
        minSdk = 21
        consumerProguardFiles("consumer-rules.pro")

        // 模拟器需要时再加 "x86_64"
        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_1_8
        targetCompatibility = JavaVersion.VERSION_1_8
    }

    buildTypes {
        getByName("release") {
            isMinifyEnabled = false
        }
    }

    // 只保留公共 API 的 lint，包私有类不参与 API 检查
    lint {
        checkReleaseBuilds = true
        abortOnError = true
    }
}
