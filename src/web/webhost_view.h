#pragma once

#include <string>

namespace logosctl::web {

// Install the desktop webview backend: a `LogosCore::WebModuleView` factory
// that runs each `web` module's page in a SEPARATE `logoscore-webhost` process
// and speaks the web transport to it over a loopback socket.
//
// WHY A CHILD PROCESS, and not a webview in this one.
//
//   1. THE LOAD PATH BLOCKS. WebContainer::awaitLoad asks the page whether it
//      published a module and waits for the answer; every inbound call to a web
//      module does the same. Those waits happen on whichever thread the core is
//      serving on, which in this daemon is the Qt main thread. A webview in
//      THIS process delivers its bridge messages on that same thread's event
//      loop, so the very first round trip would wait for a reply that only the
//      waiting thread could deliver. A socket read on a pump thread of its own
//      has no such relationship.
//   2. CRASH CONTAINMENT is the whole reason the Web container exists next to
//      the Native one (see web_container.h). A page whose process dies has to
//      leave the host running, and "the process died" has to be observable.
//      Here it is exactly one thing: the socket reaches EOF.
//   3. Qt WebEngine cannot live under a QCoreApplication, which is what a
//      headless daemon builds. Putting it in the child keeps Chromium out of
//      this binary entirely -- `logoscore` without `--container web` does not
//      link it, load it, or pay for it.
//
// `helperPath` is the webhost binary; empty means "beside this executable".
// Returns false and installs NOTHING when the helper is not there, writing the
// reason into `whyNot` when it is non-null -- a `web` module then reports the
// missing bridge exactly as it does on a host with no backend at all, which is
// the honest answer for a build that shipped without one.
bool installWebhostBackend(const std::string& helperPath = {},
                           std::string* whyNot = nullptr);

// The name of the helper binary this backend spawns.
const char* webhostBinaryName();

} // namespace logosctl::web
