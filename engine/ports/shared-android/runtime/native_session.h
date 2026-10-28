#pragma once
int native_session_requested(void);
int native_session_continuous(void);
const char *native_session_suffix(void);
void native_session_stop_idle(void);
void native_session_start(void);
void native_session_finish(void);
