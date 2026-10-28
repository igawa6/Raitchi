/* Raitchi is an offline runtime. These are guest protocol results, not host
 * Android connectivity errors; Wi-Fi availability never enables Switch online.
 * Result identities: switchbrew.org/wiki/Error_codes (Account/Nifm/Am). */
#pragma once
#define RAITCHI_OFFLINE_NETWORK_DISABLED MAKE_RESULT(110, 1111)
#define RAITCHI_OFFLINE_ACCOUNT_REQUEST MAKE_RESULT(124, 59)
#define RAITCHI_OFFLINE_ACCOUNT_UNLINKED MAKE_RESULT(124, 200)
#define RAITCHI_OFFLINE_ACCOUNT_CANCELLED MAKE_RESULT(124, 1)
#define RAITCHI_OFFLINE_UNSUPPORTED MAKE_RESULT(1, 127)
