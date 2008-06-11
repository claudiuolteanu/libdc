#ifndef UWATEC_ALADIN_H
#define UWATEC_ALADIN_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct aladin aladin;

#define UWATEC_ALADIN_MEMORY_SIZE 2048

int uwatec_aladin_open (aladin **device, const char* name);

int uwatec_aladin_close (aladin *device);

int uwatec_aladin_read (aladin *device, unsigned char data[], unsigned int size);

int uwatec_aladin_extract_dives (const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* UWATEC_ALADIN_H */