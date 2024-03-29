#!/bin/bash -e

# SPDX-FileCopyrightText: 2024 uzuy Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

set -x

echo -e "\e[1m\e[33mBuild tools information:\e[0m"
cmake --version
gcc -v
tx --version

mkdir build && cd build
cmake .. -DENABLE_QT_TRANSLATION=ON -DGENERATE_QT_TRANSLATION=ON -DCMAKE_BUILD_TYPE=Release -DENABLE_SDL2=OFF -DUZUY_TESTS=OFF -DUZUY_USE_BUNDLED_VCPKG=ON
make translation
cd ..

cd dist/languages
tx push -s
