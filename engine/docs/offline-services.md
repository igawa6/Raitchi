# Offline service guard

Raitchi declines unsupported Switch online requests independently of Android's
Wi-Fi state. The common runtime previously had unregistered NIFM interfaces and
an applet creator falling through to success without returning an object. Its
local account manager also reported availability and fabricated an online account
ID. These are concrete source defects, not a reproduced cause of every reported
online-menu crash.

The runtime now provides NIFM general/request objects, disconnected queries,
completion events and terminal offline results. Account calls preserve the exact
local user/save identity and refuse online account/token/authentication requests.
NetConnect completes offline. ProfileSelect parses its input settings, selecting
the existing local user for a valid local request and returning cancellation with
a zero UID when an online account is required. Error applets acknowledge completion.
Unknown protocols return an unsupported result rather than success with no object.

The applet storage path resolves incoming service objects through the caller's
domain or moved session handles. Non-service handles are not cast to storage.
Request/applet services release their own event handle slots; independent guest
copies remain usable. Closing an already-recycled slot cannot close a different
object. Kernel objects remain process-owned, consistent with existing waiter
lifetime rules.

Guest result and protocol identities were checked against
[Switchbrew account services](https://switchbrew.org/wiki/Account_services),
[error codes](https://switchbrew.org/wiki/Error_codes), and the pinned local Eden
NIFM/account/applet implementations. The implementation was written locally.

`tools/check_offline_services.py` exercises actual IPC dispatch and registered
handlers using synthetic requests/kernel objects. Its 1049 assertions pass in
optimized and ASan/UBSan builds, including moved/domain/TIPC storage, malformed
inputs, unchanged local UID, online cancellation, event cleanup and surviving
guest copies. Existing IPC/audio protocol checks also pass. No Thor game-level
online reproduction was performed; direct socket, browser and other unknown
applet protocols remain outside this change.
