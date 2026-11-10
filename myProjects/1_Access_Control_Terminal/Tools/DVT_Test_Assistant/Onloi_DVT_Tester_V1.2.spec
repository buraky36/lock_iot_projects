# -*- mode: python ; coding: utf-8 -*-


a = Analysis(
    ['C:/Users/gentek-ideapad/Documents/Presents/lock_iot_projects/myProjects/1_Access_Control_Terminal/Tools/DVT_Test_Assistant/onloi_dvt_assistant.py'],
    pathex=[],
    binaries=[],
    datas=[('C:/Users/gentek-ideapad/Documents/Presents/lock_iot_projects/myProjects/1_Access_Control_Terminal/Tools/DVT_Test_Assistant/app_icon.ico', '.')],
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
    name='Onloi_DVT_Tester_V1.2',
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
    version='C:/Users/gentek-ideapad/Documents/Presents/lock_iot_projects/myProjects/1_Access_Control_Terminal/Tools/DVT_Test_Assistant/file_version_info.txt',
    icon=['C:/Users/gentek-ideapad/Documents/Presents/lock_iot_projects/myProjects/1_Access_Control_Terminal/Tools/DVT_Test_Assistant/app_icon.ico'],
)
