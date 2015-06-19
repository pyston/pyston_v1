from warnings import warnpy3k
warnpy3k("the AL module has been removed in Python 3.0", stacklevel=2)
del warnpy3k

RATE_48000      = 48000
RATE_44100      = 44100
RATE_32000      = 32000
RATE_22050      = 22050
RATE_16000      = 16000
RATE_11025      = 11025
RATE_8000       = 8000

SAMPFMT_TWOSCOMP= 1
SAMPFMT_FLOAT   = 32
SAMPFMT_DOUBLE  = 64

SAMPLE_8        = 1
SAMPLE_16       = 2
        # SAMPLE_24 is the low 24 bits of a long, sign extended to 32 bits
SAMPLE_24       = 4

MONO            = 1
STEREO          = 2
QUADRO          = 4                     # 4CHANNEL is not a legal Python name

INPUT_LINE      = 0
INPUT_MIC       = 1
INPUT_DIGITAL   = 2

MONITOR_OFF     = 0
MONITOR_ON      = 1

ERROR_NUMBER            = 0
ERROR_TYPE              = 1
ERROR_LOCATION_LSP      = 2
ERROR_LOCATION_MSP      = 3
ERROR_LENGTH            = 4

ERROR_INPUT_UNDERFLOW   = 0
ERROR_OUTPUT_OVERFLOW   = 1

# These seem to be not supported anymore:
##HOLD, RELEASE                 = 0, 1
##ATTAIL, ATHEAD, ATMARK, ATTIME        = 0, 1, 2, 3

DEFAULT_DEVICE  = 1

INPUT_SOURCE            = 0
LEFT_INPUT_ATTEN        = 1
RIGHT_INPUT_ATTEN       = 2
INPUT_RATE              = 3
OUTPUT_RATE             = 4
LEFT_SPEAKER_GAIN       = 5
RIGHT_SPEAKER_GAIN      = 6
INPUT_COUNT             = 7
OUTPUT_COUNT            = 8
UNUSED_COUNT            = 9
SYNC_INPUT_TO_AES       = 10
SYNC_OUTPUT_TO_AES      = 11
MONITOR_CTL             = 12
LEFT_MONITOR_ATTEN      = 13
RIGHT_MONITOR_ATTEN     = 14

ENUM_VALUE      = 0     # only certain values are valid
RANGE_VALUE     = 1     # any value in range is valid
