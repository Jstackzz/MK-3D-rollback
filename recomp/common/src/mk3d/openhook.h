#pragma once
// The runtime takes one say over which host file an Xbox path opens (kernel::SetOpenHook); more than one part of a
// game wants one (the jukebox's sound banks, Deadly Alliance's costumes), so each adds its hook here and the first
// that takes a path wins. Same rules as the runtime's hook: called with the normalised Xbox path in upper case and the
// host path the title's files give it (empty on a disc image), under the runtime's lock: no opens through the kernel.
#include <string>

namespace mk3d {

using OpenHook = bool (*)(const std::string& xbox_upper, std::string& host);
void AddOpenHook(OpenHook hook);

}  // namespace mk3d
