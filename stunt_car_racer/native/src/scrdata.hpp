// scrdata.hpp — lecture des données de Stunt Car Racer (Atari ST) depuis l'image disque
// de l'utilisateur : FAT12, décompression JEK Packer (ByteKiller), programme du jeu.
#pragma once
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace scr {

using Bytes = std::vector<uint8_t>;

std::map<std::string, Bytes> fatList(const Bytes &img);
// décompression arrière ByteKiller ; ok = checksum nul
Bytes byteKiller(const Bytes &buf, size_t end, bool &ok);
Bytes unpackPrg(const Bytes &prg, bool &ok);
// programme du jeu (exécutable interne de GAME.PUT) depuis une image .st ou GAME.PUT
Bytes innerProgram(const Bytes &file);
// TEXT+DATA relogés pour une base donnée ; bss = taille du BSS
Bytes relocate(const Bytes &prg, uint32_t base, uint32_t &bss);

Bytes readFile(const std::string &path);

}  // namespace scr
