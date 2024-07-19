#pragma once

#ifdef __cplusplus
extern "C" {
#endif
extern void indicator_init();

extern void indicator_show(int timeout_ms, int indicator_size);

#ifdef __cplusplus
}
#endif