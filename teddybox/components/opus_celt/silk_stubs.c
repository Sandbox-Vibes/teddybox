/* Tonie files are CELT only (as in the original firmware's decoder, which has
   no SILK part): these stand in for the SILK decoder. A SILK or hybrid packet
   makes opus_decode() fail instead of being decoded. */
#include "opus_types.h"
#include "silk/API.h"

opus_int silk_Get_Decoder_Size(opus_int *decSizeBytes)
{
    *decSizeBytes = 0;
    return 0;
}

opus_int silk_ResetDecoder(void *decState)
{
    return 0;
}

opus_int silk_InitDecoder(void *decState)
{
    return 0;
}

opus_int silk_Decode(void *decState, silk_DecControlStruct *decControl, opus_int lostFlag, opus_int newPacketFlag,
                     ec_dec *psRangeDec, opus_int16 *samplesOut, opus_int32 *nSamplesOut, int arch)
{
    return -1;
}
