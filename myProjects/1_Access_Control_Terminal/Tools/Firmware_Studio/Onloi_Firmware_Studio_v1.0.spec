# -*- mode: python ; coding: utf-8 -*-


a = Analysis(
    ['C:/Users/gentek-ideapad/Documents/Presents/lock_iot_projects/myProjects/1_Access_Control_Terminal/Tools/Firmware_Studio/onloi_firmware_studio.py'],
    pathex=[],
    binaries=[],
    datas=[('C:/Users/gentek-ideapad/Documents/Presents/lock_iot_projects/myProjects/1_Access_Control_Terminal/Tools/Firmware_Studio/studio_icon.ico', '.')],
    hiddenimports=[],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='Onloi_Firmware_Studio_v1.0',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    version='C:/Users/gentek-ideapad/Documents/Presents/lock_iot_projects/myProjects/1_Access_Control_Terminal/Tools/Firmware_Studio/file_version_info.txt',
    icon=['C:/Users/gentek-ideapad/Documents/Presents/lock_iot_projects/myProjects/1_Access_Control_Terminal/Tools/Firmware_Studio/studio_icon.ico'],
)
