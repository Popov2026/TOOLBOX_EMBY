"""Genere le fichier de ressources de version Windows lu par PyInstaller.

Ces metadonnees (editeur, produit, version) apparaissent dans
Proprietes > Details de EmbyToolbox.exe. Elles ne remplacent PAS une
signature de code : seul un certificat de confiance fait afficher un
"editeur verifie" par Windows.

Usage : python packaging/make_version_file.py <version> <sortie>
"""
import re
import sys

APP = "Emby Toolbox"
COMPANY = "Popov2026"
EXE = "EmbyToolbox.exe"


def main():
    version = (sys.argv[1] if len(sys.argv) > 1 else "0.0.0").lstrip("vV")
    out = sys.argv[2] if len(sys.argv) > 2 else "version_info.txt"
    nums = [int(x) for x in re.findall(r"\d+", version)][:4]
    nums += [0] * (4 - len(nums))
    tup = "(%d, %d, %d, %d)" % tuple(nums)
    text = f"""VSVersionInfo(
  ffi=FixedFileInfo(filevers={tup}, prodvers={tup}, mask=0x3f, flags=0x0,
                    OS=0x40004, fileType=0x1, subtype=0x0, date=(0, 0)),
  kids=[
    StringFileInfo([StringTable('040C04B0', [
      StringStruct('CompanyName', '{COMPANY}'),
      StringStruct('FileDescription', '{APP} - outils pour serveur Emby'),
      StringStruct('FileVersion', '{version}'),
      StringStruct('InternalName', 'EmbyToolbox'),
      StringStruct('LegalCopyright', '(c) {COMPANY}'),
      StringStruct('OriginalFilename', '{EXE}'),
      StringStruct('ProductName', '{APP}'),
      StringStruct('ProductVersion', '{version}')])]),
    VarFileInfo([VarStruct('Translation', [0x040C, 1200])])
  ]
)
"""
    with open(out, "w", encoding="utf-8") as fh:
        fh.write(text)
    print("version file:", out, version)


if __name__ == "__main__":
    main()
