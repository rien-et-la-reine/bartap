/* One object per caller-owned type, each exactly sizeof that type, so nm on the cross-compiled object
 * reports the target's sizes without running anything (R51's sizeof report). Test code only. */
#include "bartap.h"

#define SIZE(type) const unsigned char size_##type[sizeof(type)] = {0};

SIZE(bartap_book_t)
SIZE(bartap_open_work_t)
SIZE(bartap_spine_item_t)
SIZE(bartap_text_t)
SIZE(bartap_toc_t)
SIZE(bartap_rolling_t)
SIZE(bartap_resource_t)
SIZE(bartap_verify_t)
SIZE(bartap_record_t)
SIZE(bartap_bookmark_t)
SIZE(bartap_metadata_t)
SIZE(bartap_source_t)
SIZE(bartap_xml_t)
SIZE(bartap_stream_t)
