# SPDX-FileCopyrightText: 2024 uzuy Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

function(copy_uzuy_SDL_deps target_dir)
    include(WindowsCopyFiles)
    set(DLL_DEST "$<TARGET_FILE_DIR:${target_dir}>/")
    windows_copy_files(${target_dir} ${SDL2_DLL_DIR} ${DLL_DEST} SDL2.dll)
endfunction(copy_uzuy_SDL_deps)
