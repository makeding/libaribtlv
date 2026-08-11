#include <aribtlv/aribtlv.h>

int main(void)
{
    aribtlv_callbacks callbacks;
    aribtlv_config config;
    aribtlv_callbacks_init(&callbacks);
    aribtlv_config_init(&config);
    aribtlv_demuxer *demuxer = aribtlv_demuxer_create(&config, &callbacks, 0);
    if (!demuxer || aribtlv_version() != ARIBTLV_VERSION_INT)
        return 1;
    aribtlv_demuxer_destroy(demuxer);
    return 0;
}
