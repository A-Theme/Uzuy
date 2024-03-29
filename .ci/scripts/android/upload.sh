#!/bin/bash -ex

# SPDX-FileCopyrightText: 2024 uzuy Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

. ./.ci/scripts/common/pre-upload.sh

REV_NAME="uzuy-${GITDATE}-${GITREV}"

BUILD_FLAVOR="mainline"

BUILD_TYPE_LOWER="release"
BUILD_TYPE_UPPER="Release"
if [ "${GITHUB_REPOSITORY}" == "uzuy-emu/uzuy" ]; then
    BUILD_TYPE_LOWER="relWithDebInfo"
    BUILD_TYPE_UPPER="RelWithDebInfo"
fi

cp src/android/app/build/outputs/apk/"${BUILD_FLAVOR}/${BUILD_TYPE_LOWER}/app-${BUILD_FLAVOR}-${BUILD_TYPE_LOWER}.apk" \
  "artifacts/${REV_NAME}.apk"
cp src/android/app/build/outputs/bundle/"${BUILD_FLAVOR}${BUILD_TYPE_UPPER}"/"app-${BUILD_FLAVOR}-${BUILD_TYPE_LOWER}.aab" \
  "artifacts/${REV_NAME}.aab"
