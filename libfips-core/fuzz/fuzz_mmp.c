#include <fips/proto/mmp/wire.h>
#include <stdlib.h>
#include <string.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint8_t buf[68], again[68];
    fips_reader r; fips_writer w;
    fips_proto_error error;
    {
        fips_mmp_sender_report a={0}, b;
        fips_reader_init(&r, data, size);
        if (fips_mmp_sender_report_decode(&r, &a, &error)==FIPS_OK) {
            if (r.pos!=47) abort();
            fips_writer_init(&w, buf, sizeof(buf));
            if (fips_mmp_sender_report_encode(&a, &w, NULL)!=FIPS_OK || w.pos!=48) abort();
            fips_reader_init(&r, buf+1, 47);
            if (fips_mmp_sender_report_decode(&r, &b, NULL)!=FIPS_OK) abort();
            fips_writer_init(&w, again, sizeof(again));
            if (fips_mmp_sender_report_encode(&b, &w, NULL)!=FIPS_OK || memcmp(buf, again, 48)) abort();
        } else if (r.pos) abort();
        size_t capacity=size ? data[0]%49 : 0;
        memset(buf, 0xa5, sizeof(buf));
        fips_writer_init(&w, buf, capacity);
        fips_status status=fips_mmp_sender_report_encode(&a, &w, &error);
        if (capacity<48) {
            if (status!=FIPS_ERR_RANGE || w.pos) abort();
            for (size_t i=0; i<sizeof(buf); ++i) if (buf[i]!=0xa5) abort();
        } else if (status!=FIPS_OK || w.pos!=48) abort();
    }
    {
        fips_mmp_session_sender_report a={0}, b;
        fips_reader_init(&r, data, size);
        if (fips_mmp_session_sender_report_decode(&r, &a, &error)==FIPS_OK) {
            if (r.pos!=46) abort();
            fips_writer_init(&w, buf, sizeof(buf));
            if (fips_mmp_session_sender_report_encode(&a, &w, NULL)!=FIPS_OK || w.pos!=46) abort();
            fips_reader_init(&r, buf+0, 46);
            if (fips_mmp_session_sender_report_decode(&r, &b, NULL)!=FIPS_OK) abort();
            fips_writer_init(&w, again, sizeof(again));
            if (fips_mmp_session_sender_report_encode(&b, &w, NULL)!=FIPS_OK || memcmp(buf, again, 46)) abort();
        } else if (r.pos) abort();
        size_t capacity=size ? data[0]%47 : 0;
        memset(buf, 0xa5, sizeof(buf));
        fips_writer_init(&w, buf, capacity);
        fips_status status=fips_mmp_session_sender_report_encode(&a, &w, &error);
        if (capacity<46) {
            if (status!=FIPS_ERR_RANGE || w.pos) abort();
            for (size_t i=0; i<sizeof(buf); ++i) if (buf[i]!=0xa5) abort();
        } else if (status!=FIPS_OK || w.pos!=46) abort();
    }
    {
        fips_mmp_receiver_report a={0}, b;
        fips_reader_init(&r, data, size);
        if (fips_mmp_receiver_report_decode(&r, &a, &error)==FIPS_OK) {
            if (r.pos!=67) abort();
            fips_writer_init(&w, buf, sizeof(buf));
            if (fips_mmp_receiver_report_encode(&a, &w, NULL)!=FIPS_OK || w.pos!=68) abort();
            fips_reader_init(&r, buf+1, 67);
            if (fips_mmp_receiver_report_decode(&r, &b, NULL)!=FIPS_OK) abort();
            fips_writer_init(&w, again, sizeof(again));
            if (fips_mmp_receiver_report_encode(&b, &w, NULL)!=FIPS_OK || memcmp(buf, again, 68)) abort();
        } else if (r.pos) abort();
        size_t capacity=size ? data[0]%69 : 0;
        memset(buf, 0xa5, sizeof(buf));
        fips_writer_init(&w, buf, capacity);
        fips_status status=fips_mmp_receiver_report_encode(&a, &w, &error);
        if (capacity<68) {
            if (status!=FIPS_ERR_RANGE || w.pos) abort();
            for (size_t i=0; i<sizeof(buf); ++i) if (buf[i]!=0xa5) abort();
        } else if (status!=FIPS_OK || w.pos!=68) abort();
    }
    {
        fips_mmp_session_receiver_report a={0}, b;
        fips_reader_init(&r, data, size);
        if (fips_mmp_session_receiver_report_decode(&r, &a, &error)==FIPS_OK) {
            if (r.pos!=66) abort();
            fips_writer_init(&w, buf, sizeof(buf));
            if (fips_mmp_session_receiver_report_encode(&a, &w, NULL)!=FIPS_OK || w.pos!=66) abort();
            fips_reader_init(&r, buf+0, 66);
            if (fips_mmp_session_receiver_report_decode(&r, &b, NULL)!=FIPS_OK) abort();
            fips_writer_init(&w, again, sizeof(again));
            if (fips_mmp_session_receiver_report_encode(&b, &w, NULL)!=FIPS_OK || memcmp(buf, again, 66)) abort();
        } else if (r.pos) abort();
        size_t capacity=size ? data[0]%67 : 0;
        memset(buf, 0xa5, sizeof(buf));
        fips_writer_init(&w, buf, capacity);
        fips_status status=fips_mmp_session_receiver_report_encode(&a, &w, &error);
        if (capacity<66) {
            if (status!=FIPS_ERR_RANGE || w.pos) abort();
            for (size_t i=0; i<sizeof(buf); ++i) if (buf[i]!=0xa5) abort();
        } else if (status!=FIPS_OK || w.pos!=66) abort();
    }
    {
        fips_mmp_path_mtu_notification a={0}, b;
        fips_reader_init(&r, data, size);
        if (fips_mmp_path_mtu_notification_decode(&r, &a, &error)==FIPS_OK) {
            if (r.pos!=2) abort();
            fips_writer_init(&w, buf, sizeof(buf));
            if (fips_mmp_path_mtu_notification_encode(&a, &w, NULL)!=FIPS_OK || w.pos!=2) abort();
            fips_reader_init(&r, buf+0, 2);
            if (fips_mmp_path_mtu_notification_decode(&r, &b, NULL)!=FIPS_OK) abort();
            fips_writer_init(&w, again, sizeof(again));
            if (fips_mmp_path_mtu_notification_encode(&b, &w, NULL)!=FIPS_OK || memcmp(buf, again, 2)) abort();
        } else if (r.pos) abort();
        size_t capacity=size ? data[0]%3 : 0;
        memset(buf, 0xa5, sizeof(buf));
        fips_writer_init(&w, buf, capacity);
        fips_status status=fips_mmp_path_mtu_notification_encode(&a, &w, &error);
        if (capacity<2) {
            if (status!=FIPS_ERR_RANGE || w.pos) abort();
            for (size_t i=0; i<sizeof(buf); ++i) if (buf[i]!=0xa5) abort();
        } else if (status!=FIPS_OK || w.pos!=2) abort();
    }
    return 0;
}
