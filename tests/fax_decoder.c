/*
 * A simple tool to decode and timestamp fax communication.
 *
 * Most of the code comes from:
 *
 * fax_decode.c - a simple FAX audio decoder
 *
 * Written by Steve Underwood <steveu@coppice.org>
 *
 */


#if defined(HAVE_CONFIG_H)
#include "config.h"
#endif

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sndfile.h>

//#if defined(WITH_SPANDSP_INTERNALS)
#define SPANDSP_EXPOSE_INTERNAL_STRUCTURES
//#endif

#include "spandsp.h"

#define SAMPLES_PER_CHUNK   160

#define DISBIT1     0x01
#define DISBIT2     0x02
#define DISBIT3     0x04
#define DISBIT4     0x08
#define DISBIT5     0x10
#define DISBIT6     0x20
#define DISBIT7     0x40
#define DISBIT8     0x80

const char *identifier;
int chunk_count = 0;
int epoch = 0;

#define MILLISECONDS_PER_CHUNK 20

static void start_log_line(char *type) {
    printf("%09u;%s;%s;", epoch, type, identifier);
}

static void write_log(const char* format, ...) {
	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
}

typedef struct
{
    int val;
    const char *str;
} value_string_t; 

static int octet_reserved_bit(char *dest,
                               const uint8_t *msg,
                               int bit_no,
                               int expected)
{
    char s[10] = ".... ....";
    int bit;
    uint8_t octet;
    
    /* Break out the octet and the bit number within it. */
    octet = msg[((bit_no - 1) >> 3) + 3];
    bit_no = (bit_no - 1) & 7;
    /* Now get the actual bit. */
    bit = (octet >> bit_no) & 1;
    /* Is it what it should be. */
    if (bit ^ expected)
    {
        /* Only log unexpected values. */
        s[7 - bit_no + ((bit_no < 4)  ?  1  :  0)] = (uint8_t) (bit + '0');
        return sprintf(dest, "  %s= Unexpected state for reserved bit: %d%%0a", s, bit);
    } else{
        return 0; 
    }
}

static int octet_bit_field(char *dest,
                            const uint8_t *msg,
                            int bit_no,
                            const char *desc,
                            const char *yeah,
                            const char *neigh)
{
    char s[10] = ".... ....";
    int bit;
    uint8_t octet;
    const char *tag;

    /* Break out the octet and the bit number within it. */
    octet = msg[((bit_no - 1) >> 3) + 3];
    bit_no = (bit_no - 1) & 7;
    /* Now get the actual bit. */
    bit = (octet >> bit_no) & 1;
    /* Edit the bit string for display. */
    s[7 - bit_no + ((bit_no < 4)  ?  1  :  0)] = (uint8_t) (bit + '0');
    /* Find the right tag to display. */
    if (bit)
    {
        if ((tag = yeah) == NULL)
            tag = "Set";
    }
    else
    {
        if ((tag = neigh) == NULL)
            tag = "Not set";
    }
    /* Eh, voila! */
    return sprintf(dest, "  %s= %s: %s%%0a", s, desc, tag);
}

static int octet_field(char *dest,
                        const uint8_t *msg,
                        int start,
                        int end,
                        const char *desc,
                        const value_string_t tags[])
{
    char s[10] = ".... ....";
    int i;
    uint8_t octet;
    const char *tag;
    
    /* Break out the octet and the bit number range within it. */
    octet = msg[((start - 1) >> 3) + 3];
    start = (start - 1) & 7;
    end = ((end - 1) & 7) + 1;

    /* Edit the bit string for display. */
    for (i = start;  i < end;  i++)
        s[7 - i + ((i < 4)  ?  1  :  0)] = (uint8_t) ((octet >> i) & 1) + '0';

    /* Find the right tag to display. */
    octet = (uint8_t) ((octet >> start) & ((0xFF + (1 << (end - start))) & 0xFF));
    tag = "Invalid";
    for (i = 0;  tags[i].str;  i++)
    {
        if (octet == tags[i].val)
        {
            tag = tags[i].str;
            break;
        }
    }
    /* Eh, voila! */
    return sprintf(dest, "  %s= %s: %s%%0a", s, desc, tag);
}

int t30_decode_dis_dtc_dcs_to_buff(char *dest, const uint8_t *pkt, int len)
{
    uint8_t frame_type;
    static const value_string_t available_signalling_rate_tags[] =
    {
        { 0x00, "V.27 ter fall-back mode" },
        { 0x01, "V.29" },
        { 0x02, "V.27 ter" },
        { 0x03, "V.27 ter and V.29" },
        { 0x0B, "V.27 ter, V.29, and V.17" },
        { 0x06, "Reserved" },
        { 0x0A, "Reserved" },
        { 0x0E, "Reserved" },
        { 0x0F, "Reserved" },
        { 0x04, "Not used" },
        { 0x05, "Not used" },
        { 0x08, "Not used" },
        { 0x09, "Not used" },
        { 0x0C, "Not used" },
        { 0x0D, "Not used" },
        { 0x00, NULL }
    };
    static const value_string_t selected_signalling_rate_tags[] =
    {
        { 0x00, "V.27ter 2400bps" },
        { 0x01, "V.29, 9600bps" },
        { 0x02, "V.27ter 4800bps" },
        { 0x03, "V.29 7200bps" },
        { 0x08, "V.17 14400bps" },
        { 0x09, "V.17 9600bps" },
        { 0x0A, "V.17 12000bps" },
        { 0x0B, "V.17 7200bps" },
        { 0x05, "Reserved" },
        { 0x07, "Reserved" },
        { 0x0C, "Reserved" },
        { 0x0D, "Reserved" },
        { 0x0E, "Reserved" },
        { 0x0F, "Reserved" },
        { 0x00, NULL }
    };
    static const value_string_t available_scan_line_length_tags[] =
    {
        { 0x00, "215mm +- 1%" },
        { 0x01, "215mm +- 1% and 255mm +- 1%" },
        { 0x02, "215mm +- 1%, 255mm +- 1% and 303mm +- 1%" },
        { 0x00, NULL }
    };
    static const value_string_t selected_scan_line_length_tags[] =
    {
        { 0x00, "215mm +- 1%" },
        { 0x01, "255mm +- 1%" },
        { 0x02, "303mm +- 1%" },
        { 0x00, NULL }
    };
    static const value_string_t available_recording_length_tags[] =
    {
        { 0x00, "A4 (297mm)" },
        { 0x01, "A4 (297mm) and B4 (364mm)" },
        { 0x02, "Unlimited" },
        { 0x00, NULL }
    };
    static const value_string_t selected_recording_length_tags[] =
    {
        { 0x00, "A4 (297mm)" },
        { 0x01, "B4 (364mm)" },
        { 0x02, "Unlimited" },
        { 0x00, NULL }
    };
    static const value_string_t available_minimum_scan_line_time_tags[] =
    {
        { 0x00, "20ms at 3.85 l/mm; T7.7 = T3.85" },
        { 0x01, "5ms at 3.85 l/mm; T7.7 = T3.85" },
        { 0x02, "10ms at 3.85 l/mm; T7.7 = T3.85" },
        { 0x03, "20ms at 3.85 l/mm; T7.7 = 1/2 T3.85" },
        { 0x04, "40ms at 3.85 l/mm; T7.7 = T3.85" },
        { 0x05, "40ms at 3.85 l/mm; T7.7 = 1/2 T3.85" },
        { 0x06, "10ms at 3.85 l/mm; T7.7 = 1/2 T3.85" },
        { 0x07, "0ms at 3.85 l/mm; T7.7 = T3.85" },
        { 0x00, NULL }
    };
    static const value_string_t selected_minimum_scan_line_time_tags[] =
    {
        { 0x00, "20ms" },
        { 0x01, "5ms" },
        { 0x02, "10ms" },
        { 0x04, "40ms" },
        { 0x07, "0ms" },
        { 0x00, NULL }
    };
    static const value_string_t shared_data_memory_capacity_tags[] =
    {
        { 0x00, "Not available" },
        { 0x01, "Level 2 = 2.0 Mbytes" },
        { 0x02, "Level 1 = 1.0 Mbytes" },
        { 0x03, "Level 3 = unlimited (i.e. >= 32 Mbytes)" },
        { 0x00, NULL }
    };
    static const value_string_t t89_profile_tags[] =
    {
        { 0x00, "Not used" },
        { 0x01, "Profiles 2 and 3" },
        { 0x02, "Profile 2" },
        { 0x04, "Profile 1" },
        { 0x06, "Profile 3" },
        { 0x03, "Reserved" },
        { 0x05, "Reserved" },
        { 0x07, "Reserved" },
        { 0x00, NULL }
    };
    static const value_string_t t44_mixed_raster_content_tags[] =
    {
        { 0x00, "0" },
        { 0x01, "1" },
        { 0x02, "2" },
        { 0x32, "3" },
        { 0x04, "4" },
        { 0x05, "5" },
        { 0x06, "6" },
        { 0x07, "7" },
        { 0x00, NULL }
    };

    char *p = dest;

    frame_type = pkt[2] & 0xFE;

	/*
    if (len <= 2)
    {
        p += sprintf(p, "  Frame is short%%0a");
        return p-dest;
    }
	*/
    
    //p += sprintf(p, "%s:%%0a", t30_frametype(pkt[2]));

    if (len <= 3)
    {
        p += sprintf(p, "  Frame is short%%0a");
        return p-dest;
    }

    p += octet_bit_field(p, pkt, 1, "Store and forward Internet fax (T.37)", NULL, NULL);
    p += octet_reserved_bit(p, pkt, 2, 0);
    p += octet_bit_field(p, pkt, 3, "Real-time Internet fax (T.38)", NULL, NULL);
    p += octet_bit_field(p, pkt, 4, "3G mobile network", NULL, NULL);
    p += octet_reserved_bit(p, pkt, 5, 0);
    if (frame_type == T30_DCS)
    {
        p += octet_reserved_bit(p, pkt, 6, 0);
        p += octet_reserved_bit(p, pkt, 7, 0);
    }
    else
    {
        p += octet_bit_field(p, pkt, 6, "V.8 capabilities", NULL, NULL);
        p += octet_bit_field(p, pkt, 7, "Preferred octets", "64 octets", "256 octets");
    }
    p += octet_reserved_bit(p, pkt, 8, 0);
    if (len <= 4)
    {
        p += sprintf(dest, "  Frame is short%%0a");
        return p-dest;
    }
    
    if (frame_type == T30_DCS)
    {
        p += octet_reserved_bit(p, pkt, 9, 0);
        p += octet_bit_field(p, pkt, 10, "Receive fax", NULL, NULL);
        p += octet_field(p, pkt, 11, 14, "Selected data signalling rate", selected_signalling_rate_tags);
    }
    else
    {
        p += octet_bit_field(p, pkt, 9, "Ready to transmit a fax document (polling)", NULL, NULL);
        p += octet_bit_field(p, pkt, 10, "Can receive fax", NULL, NULL);
        p += octet_field(p, pkt, 11, 14, "Supported data signalling rates", available_signalling_rate_tags);
    }
    p += octet_bit_field(p, pkt, 15, "R8x7.7lines/mm and/or 200x200pels/25.4mm", NULL, NULL);
    p += octet_bit_field(p, pkt, 16, "2-D coding", NULL, NULL);
    if (len <= 5)
    {
        p += sprintf(dest, "  Frame is short%%0a");
        return p-dest;
    }

    if (frame_type == T30_DCS)
    {
        p += octet_field(p, pkt, 17, 18, "Recording width", selected_scan_line_length_tags);
        p += octet_field(p, pkt, 19, 20, "Recording length", selected_recording_length_tags);
        p += octet_field(p, pkt, 21, 23, "Minimum scan line time", selected_minimum_scan_line_time_tags);
    }
    else
    {
        p += octet_field(p, pkt, 17, 18, "Recording width", available_scan_line_length_tags);
        p += octet_field(p, pkt, 19, 20, "Recording length", available_recording_length_tags);
        p += octet_field(p, pkt, 21, 23, "Receiver's minimum scan line time", available_minimum_scan_line_time_tags);
    }
    p += octet_bit_field(p, pkt, 24, "Extension indicator", NULL, NULL);
    if (!(pkt[5] & DISBIT8))
        return p-dest;
    if (len <= 6)
    {
        p += sprintf(dest, "  Frame is short%%0a");
        return p-dest;
    }

    p += octet_reserved_bit(p, pkt, 25, 0);
    p += octet_bit_field(p, pkt, 26, "Compressed/uncompressed mode", "Uncompressed", "Compressed");
    p += octet_bit_field(p, pkt, 27, "Error correction mode (ECM)", "ECM", "Non-ECM");
    if (frame_type == T30_DCS)
        p += octet_bit_field(p, pkt, 28, "Frame size", "64 octets", "256 octets");
    else
        p += octet_reserved_bit(p, pkt, 28, 0);
    p += octet_reserved_bit(p, pkt, 29, 0);
    p += octet_reserved_bit(p, pkt, 30, 0);
    p += octet_bit_field(p, pkt, 31, "T.6 coding", NULL, NULL);
    p += octet_bit_field(p, pkt, 32, "Extension indicator", NULL, NULL);
    if (!(pkt[6] & DISBIT8))
        return p-dest;
    if (len <= 7)
    {
        p += sprintf(dest, "  Frame is short%%0a");
        return p-dest;
    }

    p += octet_bit_field(p, pkt, 33, "\"Field not valid\" supported", NULL, NULL);
    if (frame_type == T30_DCS)
    {
        p += octet_reserved_bit(p, pkt, 34, 0);
        p += octet_reserved_bit(p, pkt, 35, 0);
    }
    else
    {
        p += octet_bit_field(p, pkt, 34, "Multiple selective polling", NULL, NULL);
        p += octet_bit_field(p, pkt, 35, "Polled sub-address", NULL, NULL);
    }
    p += octet_bit_field(p, pkt, 36, "T.43 coding", NULL, NULL);
    p += octet_bit_field(p, pkt, 37, "Plane interleave", NULL, NULL);
    p += octet_bit_field(p, pkt, 38, "Voice coding with 32kbit/s ADPCM (Rec. G.726)", NULL, NULL);
    p += octet_bit_field(p, pkt, 39, "Reserved for the use of extended voice coding set", NULL, NULL);
    p += octet_bit_field(p, pkt, 40, "Extension indicator", NULL, NULL);
    if (!(pkt[7] & DISBIT8))
        return p-dest;
    if (len <= 8)
    {
        p += sprintf(dest, "  Frame is short%%0a");
        return p-dest;
    }
    p += octet_bit_field(p, pkt, 41, "R8x15.4lines/mm", NULL, NULL);
    p += octet_bit_field(p, pkt, 42, "300x300pels/25.4mm", NULL, NULL);
    p += octet_bit_field(p, pkt, 43, "R16x15.4lines/mm and/or 400x400pels/25.4mm", NULL, NULL);
    if (frame_type == T30_DCS)
    {
        p += octet_bit_field(p, pkt, 44, "Resolution type selection", "Inch", "Metric");
        p += octet_reserved_bit(p, pkt, 45, 0);
        p += octet_reserved_bit(p, pkt, 46, 0);
        p += octet_reserved_bit(p, pkt, 47, 0);
    }
    else
    {
        p += octet_bit_field(p, pkt, 44, "Inch-based resolution preferred", NULL, NULL);
        p += octet_bit_field(p, pkt, 45, "Metric-based resolution preferred", NULL, NULL);
        p += octet_bit_field(p, pkt, 46, "Minimum scan line time for higher resolutions", "T15.4 = 1/2 T7.7", "T15.4 = T7.7");
        p += octet_bit_field(p, pkt, 47, "Selective polling", NULL, NULL);
    }
    p += octet_bit_field(p, pkt, 48, "Extension indicator", NULL, NULL);
    if (!(pkt[8] & DISBIT8))
        return p-dest;
    if (len <= 9)
    {
        p += sprintf(dest, "  Frame is short%%0a");
        return p-dest;
    }

    p += octet_bit_field(p, pkt, 49, "Sub-addressing", NULL, NULL);
    if (frame_type == T30_DCS)
    {
        p += octet_bit_field(p, pkt, 50, "Sender identification transmission", NULL, NULL);
        p += octet_reserved_bit(p, pkt, 51, 0);
    }
    else
    {
        p += octet_bit_field(p, pkt, 50, "Password", NULL, NULL);
        p += octet_bit_field(p, pkt, 51, "Ready to transmit a data file (polling)", NULL, NULL);
    }
    p += octet_reserved_bit(p, pkt, 52, 0);
    p += octet_bit_field(p, pkt, 53, "Binary file transfer (BFT)", NULL, NULL);
    p += octet_bit_field(p, pkt, 54, "Document transfer mode (DTM)", NULL, NULL);
    p += octet_bit_field(p, pkt, 55, "Electronic data interchange (EDI)", NULL, NULL);
    p += octet_bit_field(p, pkt, 56, "Extension indicator", NULL, NULL);
    if (!(pkt[9] & DISBIT8))
        return p-dest;
    if (len <= 10)
    {
        p += sprintf(dest, "  Frame is short%%0a");
        return p-dest;
    }

    p += octet_bit_field(p, pkt, 57, "Basic transfer mode (BTM)", NULL, NULL);
    p += octet_reserved_bit(p, pkt, 58, 0);
    if (frame_type == T30_DCS)
        p += octet_reserved_bit(p, pkt, 59, 0);
    else
        p += octet_bit_field(p, pkt, 59, "Ready to transfer a character or mixed mode document (polling)", NULL, NULL);
    p += octet_bit_field(p, pkt, 60, "Character mode", NULL, NULL);
    p += octet_reserved_bit(p, pkt, 61, 0);
    p += octet_bit_field(p, pkt, 62, "Mixed mode (Annex E/T.4)", NULL, NULL);
    p += octet_reserved_bit(p, pkt, 63, 0);
    p += octet_bit_field(p, pkt, 64, "Extension indicator", NULL, NULL);
    if (!(pkt[10] & DISBIT8))
        return p-dest;
    if (len <= 11)
    {
        p += sprintf(dest, "  Frame is short%%0a");
        return p-dest;
    }

    p += octet_bit_field(p, pkt, 65, "Processable mode 26 (Rec. T.505)", NULL, NULL);
    p += octet_bit_field(p, pkt, 66, "Digital network capability", NULL, NULL);
    p += octet_bit_field(p, pkt, 67, "Duplex capability", "Full", "Half only");
    if (frame_type == T30_DCS)
        p += octet_bit_field(p, pkt, 68, "Full colour mode", NULL, NULL);
    else
        p += octet_bit_field(p, pkt, 68, "JPEG coding", NULL, NULL);
    p += octet_bit_field(p, pkt, 69, "Full colour mode", NULL, NULL);
    if (frame_type == T30_DCS)
        p += octet_bit_field(p, pkt, 70, "Preferred Huffman tables", NULL, NULL);
    else
        p += octet_reserved_bit(p, pkt, 70, 0);
    p += octet_bit_field(p, pkt, 71, "12bits/pel component", NULL, NULL);
    p += octet_bit_field(p, pkt, 72, "Extension indicator", NULL, NULL);
    if (!(pkt[11] & DISBIT8))
        return p-dest;
    if (len <= 12)
    {
        p += sprintf(dest, "  Frame is short%%0a");
        return p-dest;
    }

    p += octet_bit_field(p, pkt, 73, "No subsampling (1:1:1)", NULL, NULL);
    p += octet_bit_field(p, pkt, 74, "Custom illuminant", NULL, NULL);
    p += octet_bit_field(p, pkt, 75, "Custom gamut range", NULL, NULL);
    p += octet_bit_field(p, pkt, 76, "North American Letter (215.9mm x 279.4mm)", NULL, NULL);
    p += octet_bit_field(p, pkt, 77, "North American Legal (215.9mm x 355.6mm)", NULL, NULL);
    p += octet_bit_field(p, pkt, 78, "Single-progression sequential coding (Rec. T.85) basic", NULL, NULL);
    p += octet_bit_field(p, pkt, 79, "Single-progression sequential coding (Rec. T.85) optional L0", NULL, NULL);
    p += octet_bit_field(p, pkt, 80, "Extension indicator", NULL, NULL);
    if (!(pkt[12] & DISBIT8))
        return p-dest;
    if (len <= 13)
    {
        p += sprintf(dest, "  Frame is short%%0a");
        return p-dest;
    }

    p += octet_bit_field(p, pkt, 81, "HKM key management", NULL, NULL);
    p += octet_bit_field(p, pkt, 82, "RSA key management", NULL, NULL);
    p += octet_bit_field(p, pkt, 83, "Override", NULL, NULL);
    p += octet_bit_field(p, pkt, 84, "HFX40 cipher", NULL, NULL);
    p += octet_bit_field(p, pkt, 85, "Alternative cipher number 2", NULL, NULL);
    p += octet_bit_field(p, pkt, 86, "Alternative cipher number 3", NULL, NULL);
    p += octet_bit_field(p, pkt, 87, "HFX40-I hashing", NULL, NULL);
    p += octet_bit_field(p, pkt, 88, "Extension indicator", NULL, NULL);
    if (!(pkt[13] & DISBIT8))
        return p-dest;
    if (len <= 14)
    {
        p += sprintf(dest, "  Frame is short%%0a");
        return p-dest;
    }

    p += octet_bit_field(p, pkt, 89, "Alternative hashing system 2", NULL, NULL);
    p += octet_bit_field(p, pkt, 90, "Alternative hashing system 3", NULL, NULL);
    p += octet_bit_field(p, pkt, 91, "Reserved for future security features", NULL, NULL);
    p += octet_field(p, pkt, 92, 94, "T.44 (Mixed Raster Content)", t44_mixed_raster_content_tags);
    p += octet_bit_field(p, pkt, 95, "Page length maximum stripe size for T.44 (Mixed Raster Content)", NULL, NULL);
    p += octet_bit_field(p, pkt, 96, "Extension indicator", NULL, NULL);
    if (!(pkt[14] & DISBIT8))
        return p-dest;
    if (len <= 15)
    {
        p += sprintf(dest, "  Frame is short%%0a");
        return p-dest;
    }

    p += octet_bit_field(p, pkt, 97, "Colour/gray-scale 300pels/25.4mm x 300lines/25.4mm or 400pels/25.4mm x 400lines/25.4mm resolution", NULL, NULL);
    p += octet_bit_field(p, pkt, 98, "100pels/25.4mm x 100lines/25.4mm for colour/gray scale", NULL, NULL);
    p += octet_bit_field(p, pkt, 99, "Simple phase C BFT negotiations", NULL, NULL);
    if (frame_type == T30_DCS)
    {
        p += octet_reserved_bit(p, pkt, 100, 0);
        p += octet_reserved_bit(p, pkt, 101, 0);
    }
    else
    {
        p += octet_bit_field(p, pkt, 100, "Extended BFT Negotiations capable", NULL, NULL);
        p += octet_bit_field(p, pkt, 101, "Internet Selective Polling address (ISP)", NULL, NULL);
    }
    p += octet_bit_field(p, pkt, 102, "Internet Routing Address (IRA)", NULL, NULL);
    p += octet_reserved_bit(p, pkt, 103, 0);
    p += octet_bit_field(p, pkt, 104, "Extension indicator", NULL, NULL);
    if (!(pkt[15] & DISBIT8))
        return p-dest;
    if (len <= 16)
    {
        p += sprintf(dest, "  Frame is short%%0a");
        return p-dest;
    }

    p += octet_bit_field(p, pkt, 105, "600pels/25.4mm x 600lines/25.4mm", NULL, NULL);
    p += octet_bit_field(p, pkt, 106, "1200pels/25.4mm x 1200lines/25.4mm", NULL, NULL);
    p += octet_bit_field(p, pkt, 107, "300pels/25.4mm x 600lines/25.4mm", NULL, NULL);
    p += octet_bit_field(p, pkt, 108, "400pels/25.4mm x 800lines/25.4mm", NULL, NULL);
    p += octet_bit_field(p, pkt, 109, "600pels/25.4mm x 1200lines/25.4mm", NULL, NULL);
    p += octet_bit_field(p, pkt, 110, "Colour/gray scale 600pels/25.4mm x 600lines/25.4mm", NULL, NULL);
    p += octet_bit_field(p, pkt, 111, "Colour/gray scale 1200pels/25.4mm x 1200lines/25.4mm", NULL, NULL);
    p += octet_bit_field(p, pkt, 112, "Extension indicator", NULL, NULL);
    if (!(pkt[16] & DISBIT8))
        return p-dest;
    if (len <= 17)
    {
        p += sprintf(dest, "  Frame is short%%0a");
        return p-dest;
    }

    p += octet_bit_field(p, pkt, 113, "Double sided printing capability (alternate mode)", NULL, NULL);
    p += octet_bit_field(p, pkt, 114, "Double sided printing capability (continuous mode)", NULL, NULL);
    if (frame_type == T30_DCS)
        p += octet_bit_field(p, pkt, 115, "Black and white mixed raster content profile (MRCbw)", NULL, NULL);
    else
        p += octet_reserved_bit(p, pkt, 115, 0);
    p += octet_bit_field(p, pkt, 116, "T.45 (run length colour encoded)", NULL, NULL);
    p += octet_field(p, pkt, 117, 118, "Shared memory", shared_data_memory_capacity_tags);
    p += octet_bit_field(p, pkt, 119, "T.44 colour space", NULL, NULL);
    p += octet_bit_field(p, pkt, 120, "Extension indicator", NULL, NULL);
    if (!(pkt[17] & DISBIT8))
        return p-dest;
    if (len <= 18)
    {
        p += sprintf(dest, "  Frame is short%%0a");
        return p-dest;
    }

    p += octet_bit_field(p, pkt, 121, "Flow control capability for T.38 communication", NULL, NULL);
    p += octet_bit_field(p, pkt, 122, "K>4", NULL, NULL);
    p += octet_bit_field(p, pkt, 123, "Internet aware T.38 mode fax (not affected by data signal rate bits)", NULL, NULL);
    p += octet_field(p, pkt, 124, 126, "T.89 (Application profiles for ITU-T Rec T.88)", t89_profile_tags);
    p += octet_bit_field(p, pkt, 127, "sYCC-JPEG coding", NULL, NULL);
    p += octet_bit_field(p, pkt, 128, "Extension indicator", NULL, NULL);
    if (!(pkt[18] & DISBIT8))
        return p-dest;

    p += sprintf(dest, "  Extended beyond the current T.30 specification!%%0a");
    return p-dest;
}

enum
{
    FAX_NONE,
    FAX_V27TER_RX,
    FAX_V29_RX,
    FAX_V17_RX
};

static const struct
{
    int bit_rate;
    int modem_type;
    int which;
    uint8_t dcs_code;
} fallback_sequence[] =
{
    {14400, T30_MODEM_V17,    T30_SUPPORT_V17,    DISBIT6},
    {12000, T30_MODEM_V17,    T30_SUPPORT_V17,    (DISBIT6 | DISBIT4)},
    { 9600, T30_MODEM_V17,    T30_SUPPORT_V17,    (DISBIT6 | DISBIT3)},
    { 9600, T30_MODEM_V29,    T30_SUPPORT_V29,    DISBIT3},
    { 7200, T30_MODEM_V17,    T30_SUPPORT_V17,    (DISBIT6 | DISBIT4 | DISBIT3)},
    { 7200, T30_MODEM_V29,    T30_SUPPORT_V29,    (DISBIT4 | DISBIT3)},
    { 4800, T30_MODEM_V27TER, T30_SUPPORT_V27TER, DISBIT4},
    { 2400, T30_MODEM_V27TER, T30_SUPPORT_V27TER, 0},
    {    0, 0, 0, 0}
};

int decode_test = FALSE;
int rx_bits = 0;

t30_state_t t30_dummy;
t4_rx_state_t t4_rx_state;
int t4_up = FALSE;

hdlc_rx_state_t hdlcrx;

int fast_trained = FAX_NONE;

uint8_t ecm_data[256][260];
int16_t ecm_len[256];

int line_encoding = T4_COMPRESSION_ITU_T4_1D;
int x_resolution = T4_X_RESOLUTION_R8;
int y_resolution = T4_Y_RESOLUTION_STANDARD;
int image_width = 1728;
int octets_per_ecm_frame = 256;
int error_correcting_mode = FALSE;
int current_fallback = 0;

static int decode_20digit_msg_to_buff(char *dest, const uint8_t *pkt, int len)
{
    int p;
    int k;
    char msg[T30_MAX_IDENT_LEN + 1];

    char *ptr = dest;

    if (len > T30_MAX_IDENT_LEN + 3)
    {
        ptr += sprintf(ptr, "XXX %d %d\n", len, T30_MAX_IDENT_LEN + 1);
        msg[0] = '\0';
        return ptr-dest;
    }
    pkt += 2;
    p = len - 2;
    /* Strip trailing spaces */
    while (p > 1  &&  pkt[p - 1] == ' ')
        p--;
    /* The string is actually backwards in the message */
    k = 0;
    while (p > 1)
        msg[k++] = pkt[--p];
    msg[k] = '\0';
    ptr += sprintf(dest, "%s", msg);
    return ptr-dest;
}
/*- End of function --------------------------------------------------------*/

static void print_frame(const uint8_t *fr, int frlen)
{
    int i;
    int type;
    const char *country;
    const char *vendor;
    const char *model;

    char buff[4096];
    
    start_log_line("MESSAGE");
    write_log("%s;", t30_frametype(fr[2]));

    for (i = 2;  i < frlen;  i++) {
        write_log("%02x", fr[i]);
        if(i < frlen-1) {
            write_log(" ");
        }
    }
	write_log(";");
    type = fr[2] & 0xFE;
    if (type == T30_DIS  ||  type == T30_DTC  ||  type == T30_DCS) {
        t30_decode_dis_dtc_dcs_to_buff(buff, fr, frlen);
        write_log("%s", buff);
    } else if (type == T30_CSI  ||  type == T30_TSI  ||  type == T30_PWD  ||  type == T30_SEP  ||  type == T30_SUB  ||  type == T30_SID) {
        decode_20digit_msg_to_buff(buff, fr, frlen);
        write_log("%s", buff);
    } else if (type == T30_NSF  ||  type == T30_NSS  ||  type == T30_NSC) {
        if (t35_decode(&fr[3], frlen - 3, &country, &vendor, &model))
        {
            if (country)
                write_log("country=%s ", country);
            if (vendor)
                write_log("vendor=%s ", vendor);
            if (model)
                write_log("model=%s ", model);
        }
    }
    write_log("\n");
}
/*- End of function --------------------------------------------------------*/

static int find_fallback_entry(int dcs_code)
{
    int i;

    /* The table is short, and not searched often, so a brain-dead linear scan seems OK */
    for (i = 0;  fallback_sequence[i].bit_rate;  i++)
    {
        if (fallback_sequence[i].dcs_code == dcs_code)
            break;
    }
    if (fallback_sequence[i].bit_rate == 0)
        return -1;
    return i;
}
/*- End of function --------------------------------------------------------*/

static int check_rx_dcs(const uint8_t *msg, int len)
{
    static const int widths[3][4] =
    {
        { 864, 1024, 1216, -1}, /* R4 resolution - no longer used in recent versions of T.30 */
        {1728, 2048, 2432, -1}, /* R8 resolution */
        {3456, 4096, 4864, -1}  /* R16 resolution */
    };
    uint8_t dcs_frame[T30_MAX_DIS_DTC_DCS_LEN];

    /* Check DCS frame from remote */
    if (len < 6)
    {
        printf("Short DCS frame\n");
        return -1;
    }

    /* Make a local copy of the message, padded to the maximum possible length with zeros. This allows
       us to simply pick out the bits, without worrying about whether they were set from the remote side. */
    if (len > T30_MAX_DIS_DTC_DCS_LEN)
    {
        memcpy(dcs_frame, msg, T30_MAX_DIS_DTC_DCS_LEN);
    }
    else
    {
        memcpy(dcs_frame, msg, len);
        if (len < T30_MAX_DIS_DTC_DCS_LEN)
            memset(dcs_frame + len, 0, T30_MAX_DIS_DTC_DCS_LEN - len);
    }

    octets_per_ecm_frame = (dcs_frame[6] & DISBIT4)  ?  256  :  64;
    if ((dcs_frame[8] & DISBIT1))
        y_resolution = T4_Y_RESOLUTION_SUPERFINE;
    else if (dcs_frame[4] & DISBIT7)
        y_resolution = T4_Y_RESOLUTION_FINE;
    else
        y_resolution = T4_Y_RESOLUTION_STANDARD;
    image_width = widths[(dcs_frame[8] & DISBIT3)  ?  2  :  1][dcs_frame[5] & (DISBIT2 | DISBIT1)];

    /* Check which compression we will use. */
    if ((dcs_frame[6] & DISBIT7))
        line_encoding = T4_COMPRESSION_ITU_T6;
    else if ((dcs_frame[4] & DISBIT8))
        line_encoding = T4_COMPRESSION_ITU_T4_2D;
    else
        line_encoding = T4_COMPRESSION_ITU_T4_1D;
	start_log_line("DEBUG");
    write_log("Selected compression %d\n", line_encoding);

    if ((current_fallback = find_fallback_entry(dcs_frame[4] & (DISBIT6 | DISBIT5 | DISBIT4 | DISBIT3))) < 0)
        printf("Remote asked for a modem standard we do not support\n");
    error_correcting_mode = ((dcs_frame[6] & DISBIT3) != 0);

    //v17_rx_restart(&v17, fallback_sequence[fallback_entry].bit_rate, FALSE);
    return 0;
}
/*- End of function --------------------------------------------------------*/

static void hdlc_accept(void *user_data, const uint8_t *msg, int len, int ok)
{
    int type;
    int frame_no;
    int i;

    if (len < 0)
    {
        /* Special conditions */
		start_log_line("DEBUG");
        write_log("HDLC status is %s (%d)\n", signal_status_to_str(len), len);
        return;
    }

    if (ok)
    {
        if (msg[0] != 0xFF  ||  !(msg[1] == 0x03  ||  msg[1] == 0x13))
        {
			start_log_line("ERROR");
            write_log("Bad HDLC frame header - %02x %02x\n", msg[0], msg[1]);
            return;
        }
        print_frame(msg, len);
        type = msg[2] & 0xFE;
        switch (type)
        {
        case T4_FCD:
            if (len <= 4 + 256)
            {
                frame_no = msg[3];
                /* Just store the actual image data, and record its length */
                memcpy(&ecm_data[frame_no][0], &msg[4], len - 4);
                ecm_len[frame_no] = (int16_t) (len - 4);
            }
            break;
        case T30_DCS:
            check_rx_dcs(msg, len);
            break;
        }
    }
    else
    {
        start_log_line("ERROR");
        write_log("Bad HDLC frame;");
        for (i = 0;  i < len;  i++) {
            write_log("%02x", msg[i]);
            if(i < len-1) {
                write_log(" ");
            }
        }
        write_log("\n");
    }
}
/*- End of function --------------------------------------------------------*/

static void t4_begin(void)
{
    int i;

    //printf("Begin T.4 - %d %d %d %d\n", line_encoding, x_resolution, y_resolution, image_width);
    t4_rx_set_rx_encoding(&t4_rx_state, line_encoding);
    t4_rx_set_x_resolution(&t4_rx_state, x_resolution);
    t4_rx_set_y_resolution(&t4_rx_state, y_resolution);
    t4_rx_set_image_width(&t4_rx_state, image_width);

    t4_rx_start_page(&t4_rx_state);
    t4_up = TRUE;

    for (i = 0;  i < 256;  i++)
        ecm_len[i] = -1;
}
/*- End of function --------------------------------------------------------*/

static void t4_end(void)
{
    t4_stats_t stats;
    int i;

    if (!t4_up)
        return;
    if (error_correcting_mode)
    {
        for (i = 0;  i < 256;  i++)
        {
            if (ecm_len[i] > 0)
                t4_rx_put_chunk(&t4_rx_state, ecm_data[i], ecm_len[i]);
            write_log("%d", (ecm_len[i] <= 0)  ?  0  :  1);
        }
        write_log("\n");
    }
    t4_rx_end_page(&t4_rx_state);
    t4_rx_get_transfer_statistics(&t4_rx_state, &stats);
    start_log_line("STATS");
    write_log("Pages = %d,", stats.pages_transferred);
    write_log("Image size = %dx%d,", stats.width, stats.length);
    write_log("Image resolution = %dx%d,", stats.x_resolution, stats.y_resolution);
    write_log("Bad rows = %d,", stats.bad_rows);
    write_log("Longest bad row run = %d\n", stats.longest_bad_row_run);
    t4_up = FALSE;
}
/*- End of function --------------------------------------------------------*/

static void v21_put_bit(void *user_data, int bit)
{
    if (bit < 0)
    {
        /* Special conditions */
		start_log_line("DEBUG");	
        write_log("V.21 rx status is %s (%d)\n", signal_status_to_str(bit), bit);
        switch (bit)
        {
        case SIG_STATUS_CARRIER_DOWN:
            //t4_end();
            break;
        }
        return;
    }
    if (fast_trained == FAX_NONE)
        hdlc_rx_put_bit(&hdlcrx, bit);
    //printf("V.21 Rx bit %d - %d\n", rx_bits++, bit);
}
/*- End of function --------------------------------------------------------*/

static void v17_put_bit(void *user_data, int bit)
{
    if (bit < 0)
    {
        /* Special conditions */
		start_log_line("DEBUG");
        write_log("V.17 rx status is %s (%d)\n", signal_status_to_str(bit), bit);
        switch (bit)
        {
        case SIG_STATUS_TRAINING_SUCCEEDED:
            fast_trained = FAX_V17_RX;
            t4_begin();
            break;
        case SIG_STATUS_CARRIER_DOWN:
            t4_end();
            if (fast_trained == FAX_V17_RX)
                fast_trained = FAX_NONE;
            break;
        }
        return;
    }
    if (error_correcting_mode)
    {
        hdlc_rx_put_bit(&hdlcrx, bit);
    }
    else
    {
        if (t4_rx_put_bit(&t4_rx_state, bit))
        {
            t4_end();
			start_log_line("DEBUG");
            write_log("End of page detected\n");
        }
    }
    //printf("V.17 Rx bit %d - %d\n", rx_bits++, bit);
}
/*- End of function --------------------------------------------------------*/

static void v29_put_bit(void *user_data, int bit)
{
    if (bit < 0)
    {
        /* Special conditions */
		start_log_line("DEBUG");
        write_log("V.29 rx status is %s (%d)\n", signal_status_to_str(bit), bit);
        switch (bit)
        {
        case SIG_STATUS_TRAINING_SUCCEEDED:
            fast_trained = FAX_V29_RX;
            t4_begin();
            break;
        case SIG_STATUS_CARRIER_DOWN:
            t4_end();
            if (fast_trained == FAX_V29_RX)
                fast_trained = FAX_NONE;
            break;
        }
        return;
    }
    if (error_correcting_mode)
    {
        hdlc_rx_put_bit(&hdlcrx, bit);
    }
    else
    {
        if (t4_rx_put_bit(&t4_rx_state, bit))
        {
            t4_end();
			start_log_line("DEBUG");
            write_log("End of page detected\n");
        }
    }
    //printf("V.29 Rx bit %d - %d\n", rx_bits++, bit);
}
/*- End of function --------------------------------------------------------*/

static void v27ter_put_bit(void *user_data, int bit)
{
    if (bit < 0)
    {
        /* Special conditions */
		start_log_line("DEBUG");
        write_log("V.27ter rx status is %s (%d)\n", signal_status_to_str(bit), bit);
        switch (bit)
        {
        case SIG_STATUS_TRAINING_SUCCEEDED:
            fast_trained = FAX_V27TER_RX;
            t4_begin();
            break;
        case SIG_STATUS_CARRIER_DOWN:
            t4_end();
            if (fast_trained == FAX_V27TER_RX)
                fast_trained = FAX_NONE;
            break;
        }
        return;
    }
    if (error_correcting_mode)
    {
        hdlc_rx_put_bit(&hdlcrx, bit);
    }
    else
    {
        if (t4_rx_put_bit(&t4_rx_state, bit))
        {
            t4_end();
			start_log_line("DEBUG");
            write_log("End of page detected\n");
        }
    }
    //printf("V.27ter Rx bit %d - %d\n", rx_bits++, bit);
}
/*- End of function --------------------------------------------------------*/

void usage(void) {
	printf("\n \
Usage: file_name identifier\n \
Ex:    side1.wav side1\n");
}

int main(int argc, char *argv[])
{
    fsk_rx_state_t *fsk;
    v17_rx_state_t *v17;
    v29_rx_state_t *v29;
    v27ter_rx_state_t *v27ter;
    int16_t amp[SAMPLES_PER_CHUNK];
    SNDFILE *inhandle;
    SF_INFO info;
    int len;
    const char *filename;
    logging_state_t *logging;

    if (argc != 3) {
        usage();
        return 1;
    }

    filename = argv[1];
    identifier = argv[2];

    memset(&info, 0, sizeof(info));
    if ((inhandle = sf_open(filename, SFM_READ, &info)) == NULL)
    {
		start_log_line("DEBUG");
        write_log("    Cannot open audio file '%s' for reading\n", filename);
        exit(2);
    }
    if (info.samplerate != SAMPLE_RATE)
    {
		start_log_line("DEBUG");
        write_log("    Unexpected sample rate in audio file '%s'\n", filename);
        exit(2);
    }
    if (info.channels != 1)
    {
		start_log_line("DEBUG");
        write_log("    Unexpected number of channels in audio file '%s'\n", filename);
        exit(2);
    }

    memset(&t30_dummy, 0, sizeof(t30_dummy));
    span_log_init(&t30_dummy.logging, SPAN_LOG_FLOW, NULL);
    span_log_set_protocol(&t30_dummy.logging, "T.30");

    hdlc_rx_init(&hdlcrx, FALSE, TRUE, 5, hdlc_accept, NULL);
    fsk = fsk_rx_init(NULL, &preset_fsk_specs[FSK_V21CH2], FSK_FRAME_MODE_SYNC, v21_put_bit, NULL);
    v17 = v17_rx_init(NULL, 14400, v17_put_bit, NULL);
    v29 = v29_rx_init(NULL, 9600, v29_put_bit, NULL);
    //v29 = v29_rx_init(NULL, 7200, v29_put_bit, NULL);
    v27ter = v27ter_rx_init(NULL, 4800, v27ter_put_bit, NULL);
    fsk_rx_signal_cutoff(fsk, -45.5);
    v17_rx_signal_cutoff(v17, -45.5);
    v29_rx_signal_cutoff(v29, -45.5);
    v27ter_rx_signal_cutoff(v27ter, -40.0);

#if 1
    logging = v17_rx_get_logging_state(v17);
    span_log_init(logging, SPAN_LOG_FLOW, NULL);
    span_log_set_protocol(logging, "V.17");
    span_log_set_level(logging, SPAN_LOG_SHOW_SEVERITY | SPAN_LOG_SHOW_PROTOCOL | SPAN_LOG_SHOW_TAG | SPAN_LOG_FLOW);

    logging = v29_rx_get_logging_state(v29);
    span_log_init(logging, SPAN_LOG_FLOW, NULL);
    span_log_set_protocol(logging, "V.29");
    span_log_set_level(logging, SPAN_LOG_SHOW_SEVERITY | SPAN_LOG_SHOW_PROTOCOL | SPAN_LOG_SHOW_TAG | SPAN_LOG_FLOW);

    logging = v27ter_rx_get_logging_state(v27ter);
    span_log_init(logging, SPAN_LOG_FLOW, NULL);
    span_log_set_protocol(logging, "V.27ter");
    span_log_set_level(logging, SPAN_LOG_SHOW_SEVERITY | SPAN_LOG_SHOW_PROTOCOL | SPAN_LOG_SHOW_TAG | SPAN_LOG_FLOW);
#endif

    if (t4_rx_init(&t4_rx_state, "fax_decode.tif", T4_COMPRESSION_ITU_T4_2D) == NULL)
    {
		start_log_line("DEBUG");
        write_log("Failed to init\n");
        exit(0);
    }
        
    for (;;)
    {
        len = sf_readf_short(inhandle, amp, SAMPLES_PER_CHUNK);
        if (len < SAMPLES_PER_CHUNK)
            break;
        fsk_rx(fsk, amp, len);
        v17_rx(v17, amp, len);
        v29_rx(v29, amp, len);
        //v27ter_rx(v27ter, amp, len);
        chunk_count++;
        epoch = chunk_count*MILLISECONDS_PER_CHUNK;
    }
    t4_rx_release(&t4_rx_state);

    if (sf_close(inhandle))
    {
		start_log_line("DEBUG");
        write_log("    Cannot close audio file '%s'\n", filename);
        exit(2);
    }
    return  0;
}
/*- End of function --------------------------------------------------------*/
/*- End of file ------------------------------------------------------------*/
