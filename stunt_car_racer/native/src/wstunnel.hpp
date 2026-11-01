// wstunnel.hpp — passage par un reverse proxy web (HTTPS) : le jeu joint le serveur relais par une
// connexion WebSocket (adresse wss://hote[:port][/chemin], ou ws:// sans chiffrement). Un tunnel local
// (127.0.0.1, port choisi par le système) relie la connexion TCP habituelle du jeu à la WebSocket :
// le protocole du jeu passe tel quel dans des messages binaires. wss:// : WinHTTP (Windows : TLS et
// certificats du système) ; ws:// : partout.
#pragma once
#include <string>

namespace scr {

bool isWsUrl(const std::string &addr);   // wss://... ou ws://...

// ouvre le tunnel et rend l'adresse locale à joindre (« 127.0.0.1:port ») ; vide + err en cas d'échec.
// Le tunnel attend une connexion locale (15 s au plus), la relie au serveur puis se ferme avec elle.
std::string openWsTunnel(const std::string &url, std::string &err);

// raison de l'échec du dernier tunnel (vide si aucun) : HTTP du proxy, erreur WinHTTP...
std::string tunnelError();

}  // namespace scr
