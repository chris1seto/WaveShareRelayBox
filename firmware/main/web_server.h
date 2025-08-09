#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"

#ifndef WEB_SERVER_PORT
#define WEB_SERVER_PORT 80
#endif

#ifndef WEB_SERVER_MAX_URI_HANDLERS
#define WEB_SERVER_MAX_URI_HANDLERS 16
#endif

esp_err_t web_server_init(void);
esp_err_t web_server_start(void);
esp_err_t web_server_stop(void);

#endif // WEB_SERVER_H


