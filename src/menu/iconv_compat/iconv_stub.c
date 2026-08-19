/* Pass-through iconv stub — enough for LoadGameMenu filename helpers. */
#include "iconv.h"

#include <string.h>

iconv_t iconv_open(const char* tocode, const char* fromcode)
{
    (void)tocode;
    (void)fromcode;
    return (iconv_t)1;
}

int iconv_close(iconv_t cd)
{
    (void)cd;
    return 0;
}

size_t iconv(iconv_t cd, char** inbuf, size_t* inbytesleft, char** outbuf, size_t* outbytesleft)
{
    size_t n;
    (void)cd;
    if (!inbuf || !*inbuf || !inbytesleft || !outbuf || !*outbuf || !outbytesleft)
        return (size_t)-1;
    n = *inbytesleft;
    if (n > *outbytesleft)
        n = *outbytesleft;
    memcpy(*outbuf, *inbuf, n);
    *inbuf += n;
    *outbuf += n;
    *inbytesleft -= n;
    *outbytesleft -= n;
    return 0;
}
