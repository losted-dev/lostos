// Does a program that the server asks the speaker to run come from somebody that the speaker itself trusts?
//
// The server tells the speaker which plugin to download and which permissions to give it. A server of somebody else must not decide that: the speaker checks the signature of the package
// (the same Ed25519 signature that the registry and the panel use, see server/plugin_sign.py) and the permissions that the signed manifest declares. A package is trusted when its key is
//   - a key of the lostOS team (built into this firmware), or
//   - on the list of verified developers (plugapi.losted.dev) whose signature is checked here with a key that is built into this firmware, not revoked, and the list is in date.
// A trusted package gets the permissions that its manifest declares and the server asks for. Any other program gets only the harmless permissions: the dangerous ones
// (the network, the sound, the button, events to Home Assistant) are refused whatever the server says.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cs {

struct PluginTrust {
  bool trusted = false;                  // the signature is good, the files are those that were signed, the key is trusted
  std::string who;                       // the publisher named in the signature (to the log)
  std::vector<std::string> declared;     // the device permissions that the signed manifest declares
  std::string why;                       // when not trusted: the first thing that did not hold
};

// dir: the folder of the plugin on the speaker; it holds trust/lostos-signature.json, trust/manifest.json, trust/developers.json, trust/developers.json.sig (what the server relayed).
// program_sha: the SHA-256 (hex) of the program that was downloaded and checked. now: the time (epoch seconds), for the end date of the list of developers.
PluginTrust verify_plugin(const std::string &dir, const std::string &program_sha, int64_t now);

// Permissions that can hurt: reaching the network, playing sound, taking the button, sending events outwards.
bool dangerous_permission(const std::string &perm);

// The permissions to give: those asked (by the server) that are known, harmless for any program, and for a trusted one also those that its signed manifest declares.
std::vector<std::string> permissions_to_grant(const std::vector<std::string> &asked, const PluginTrust &t);

}  // namespace cs
