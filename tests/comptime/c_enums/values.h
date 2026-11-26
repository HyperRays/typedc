/* Enums as a C header declares them: typedc reads their values from here. */
typedef enum {
    MODE_FIRST,             /* implicit values count up from 0 */
    MODE_SECOND,
    MODE_TENTH = 10,
    MODE_ELEVENTH,          /* and go on from an explicit one */
    MODE_NEGATIVE = -1000011001,
    MODE_FLAG = 1 << 4,
    MODE_HEX = 0x7FFFFFFF,
    MODE_ALIAS = MODE_TENTH,    /* another enumerator's value */
} Mode;
