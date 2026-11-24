#!/bin/bash
set -eu

# Run from the source work tree. The generator refuses another library/source
# hash and refuses to replace an existing fixture or manifest.
dev100_qt=${QT_PREFIX:?Set QT_PREFIX to the Qt 6.8.3 macOS prefix}
dev100_sodium=${SODIUM_PREFIX:?Set SODIUM_PREFIX to the libsodium prefix}
/Library/Developer/CommandLineTools/usr/bin/c++ \
    -std=c++20 -O2 -DNDEBUG -DQT_CORE_LIB -DQT_NO_DEBUG -DFINIX_READ_ONLY_BETA=1 \
    -isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX26.2.sdk \
    -mmacosx-version-min=13.0 \
    -Ifincept-qt/src -I"$dev100_qt/lib/QtCore.framework/Headers" \
    -I"$dev100_qt/mkspecs/macx-clang" -I"$dev100_qt/include" \
    -I"$dev100_sodium/include" -F"$dev100_qt/lib" \
    fincept-qt/tests/finix/fixtures/dev99-profile/generate.cpp \
    ../build/libfinix_vault.a "$dev100_sodium/lib/libsodium.a" \
    -framework QtCore -framework DiskArbitration -framework UniformTypeIdentifiers \
    -framework Security -framework AppKit -framework ApplicationServices \
    -framework Foundation -framework IOKit -Wl,-rpath,"$dev100_qt/lib" \
    -o ../tools/dev100-generate-dev99-profile

../tools/dev100-generate-dev99-profile \
    fincept-qt/tests/finix/fixtures/dev99-profile \
    ../build/libfinix_vault.a fincept-qt/src/finix/Vault.cpp \
    cef32c29c709b0f127c4fa118ef7e9617dcd5d05
