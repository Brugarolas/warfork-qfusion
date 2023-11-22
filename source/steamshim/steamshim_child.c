#include "steamshim_child.h"
#include "pipe.c"
#include "pipe.h"
#include <string.h>
#include "steamshim_types.h"

#define DEBUGPIPE 1

static int initPipes(void)
{
    char buf[64];
    unsigned long long val;

    if (!getEnvVar("STEAMSHIM_READHANDLE", buf, sizeof (buf)))
        return 0;
    else if (sscanf(buf, "%llu", &val) != 1)
        return 0;
    else
        GPipeRead = (PipeType) val;

    if (!getEnvVar("STEAMSHIM_WRITEHANDLE", buf, sizeof (buf)))
        return 0;
    else if (sscanf(buf, "%llu", &val) != 1)
        return 0;
    else
        GPipeWrite = (PipeType) val;
    
    return ((GPipeRead != NULLPIPE) && (GPipeWrite != NULLPIPE));
} /* initPipes */


int STEAMSHIM_init(void)
{
    dbgpipe("Child init start.\n");
    if (!initPipes())
    {
        dbgpipe("Child init failed.\n");
        return 0;
    } /* if */

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    dbgpipe("Child init success!\n");
    return 1;
} /* STEAMSHIM_init */

void STEAMSHIM_deinit(void)
{
    dbgpipe("Child deinit.\n");
    if (GPipeWrite != NULLPIPE)
    {
        writeBye();
        closePipe(GPipeWrite);
    } /* if */

    if (GPipeRead != NULLPIPE)
        closePipe(GPipeRead);

    GPipeRead = GPipeWrite = NULLPIPE;

#ifndef _WIN32
    signal(SIGPIPE, SIG_DFL);
#endif
} /* STEAMSHIM_deinit */

static inline int isAlive(void)
{
    return ((GPipeRead != NULLPIPE) && (GPipeWrite != NULLPIPE));
} /* isAlive */

static inline int isDead(void)
{
    return !isAlive();
} /* isDead */

int STEAMSHIM_alive(void)
{
    return isAlive();
} /* STEAMSHIM_alive */

static const STEAMSHIM_Event *processEvent(const uint8 *buf, size_t buflen)
{
    static STEAMSHIM_Event event;
    const STEAMSHIM_EventType type = (STEAMSHIM_EventType) *(buf++);
    buflen--;

    memset(&event, '\0', sizeof (event));
    event.type = type;
    event.okay = 1;

    #if DEBUGPIPE
    if (0) {}
    #define PRINTGOTEVENT(x) else if (type == x) printf("Child got " #x ".\n")
    PRINTGOTEVENT(SHIMEVENT_BYE);
    PRINTGOTEVENT(SHIMEVENT_STATSRECEIVED);
    PRINTGOTEVENT(SHIMEVENT_STATSSTORED);
    PRINTGOTEVENT(SHIMEVENT_SETACHIEVEMENT);
    PRINTGOTEVENT(SHIMEVENT_GETACHIEVEMENT);
    PRINTGOTEVENT(SHIMEVENT_RESETSTATS);
    PRINTGOTEVENT(SHIMEVENT_SETSTATI);
    PRINTGOTEVENT(SHIMEVENT_GETSTATI);
    PRINTGOTEVENT(SHIMEVENT_SETSTATF);
    PRINTGOTEVENT(SHIMEVENT_GETSTATF);
    PRINTGOTEVENT(SHIMEVENT_STEAMIDRECIEVED);
    PRINTGOTEVENT(SHIMEVENT_PERSONANAMERECIEVED);
    PRINTGOTEVENT(SHIMEVENT_AUTHSESSIONTICKETRECIEVED);
    #undef PRINTGOTEVENT
    else printf("Child got unknown shimevent %d.\n", (int) type);
    #endif

    if (type >= SHIMEVENT_STEAMIDRECIEVED){
        event.okay = *(buf++) ? 1 : 0;
        pipebuff_t pipebuf;
        pipebuf.cursize = 0;
        memcpy(pipebuf.data, buf, buflen);
        PIPE_Read(&pipebuf);
        
        switch (type){
		    case SHIMEVENT_STEAMIDRECIEVED:
			    event.lvalue = PIPE_ReadLong();
			    break;
		    case SHIMEVENT_PERSONANAMERECIEVED:
		        strcpy(event.name,((char*)buf));
		        break;
		    case SHIMEVENT_AUTHSESSIONTICKETRECIEVED:
		        event.lvalue = PIPE_ReadLong();

                void* pTicket = PIPE_ReadData(AUTH_TICKET_MAXSIZE);
                memcpy(event.name, pTicket, AUTH_TICKET_MAXSIZE);
                break;
            default:
                return NULL;
        }

    }else
        switch (type)
        {
            case SHIMEVENT_BYE:
                break;

            case SHIMEVENT_STATSRECEIVED:
            case SHIMEVENT_STATSSTORED:
                if (!buflen) return NULL;
                event.okay = *(buf++) ? 1 : 0;
                break;

            case SHIMEVENT_SETACHIEVEMENT:
                if (buflen < 3) return NULL;
                event.ivalue = *(buf++) ? 1 : 0;
                event.okay = *(buf++) ? 1 : 0;
                strcpy(event.name, (const char *) buf);
                break;

            case SHIMEVENT_GETACHIEVEMENT:
                if (buflen < 10) return NULL;
                event.ivalue = (int) *(buf++);
                if (event.ivalue == 2)
                    event.ivalue = event.okay = 0;
                event.lvalue = (long long unsigned) *((uint64 *) buf);
                buf += sizeof (uint64);
                strcpy(event.name, (const char *) buf);
                break;

            case SHIMEVENT_RESETSTATS:
                if (buflen != 2) return NULL;
                event.ivalue = *(buf++) ? 1 : 0;
                event.okay = *(buf++) ? 1 : 0;
                break;

            case SHIMEVENT_SETSTATI:
            case SHIMEVENT_GETSTATI:
                event.okay = *(buf++) ? 1 : 0;
                event.ivalue = (int) *((int32 *) buf);
                buf += sizeof (int32);
                strcpy(event.name, (const char *) buf);
                break;

            case SHIMEVENT_SETSTATF:
            case SHIMEVENT_GETSTATF:
                event.okay = *(buf++) ? 1 : 0;
                event.fvalue = (int) *((float *) buf);
                buf += sizeof (float);
                strcpy(event.name, (const char *) buf);
                break;
            default:  /* uh oh */
                return NULL;
        } /* switch */


    

    return &event;
} /* processEvent */

const STEAMSHIM_Event *STEAMSHIM_pump(void)
{
    static uint8 buf[MAX_BUFFSIZE+sizeof(int)+sizeof(uint8)+sizeof(int)];
    static int br = 0;
    int evlen = (br > 0) ? (*(int*) buf) : 0;

    if (isDead())
        return NULL;

    if (br <= evlen)  /* we have an incomplete commmand. Try to read more. */
    {
        if (pipeReady(GPipeRead))
        {
            const int morebr = readPipe(GPipeRead, buf + br, sizeof (buf) - br);
            if (morebr > 0)
                br += morebr;
            else  /* uh oh */
            {
                dbgpipe("Child readPipe failed! Shutting down.\n");
                STEAMSHIM_deinit();   /* kill it all. */
            } /* else */
        } /* if */
    } /* if */

    if (evlen && (br > evlen))
    {
        printf("BBBBBBBBBBBBBBBBB%i\n",evlen);
        const STEAMSHIM_Event *retval = processEvent(buf+sizeof(int), evlen);
        br -= evlen + sizeof(int);
        if (br > 0)
            memmove(buf, buf+evlen+sizeof(int), br);
        return retval;
    } /* if */

    /* Run Steam event loop. */
    if (br == 0)
    {
        //dbgpipe("Child sending SHIMCMD_PUMP().\n");
        write1ByteCmd(SHIMCMD_PUMP);
    } /* if */


    char buffer[1024];

    return &buffer;
} /* STEAMSHIM_pump */

void STEAMSHIM_requestStats(void)
{
    if (isDead()) return;
    dbgpipe("Child sending SHIMCMD_REQUESTSTATS().\n");
    write1ByteCmd(SHIMCMD_REQUESTSTATS);
} /* STEAMSHIM_requestStats */

void STEAMSHIM_storeStats(void)
{
    if (isDead()) return;
    dbgpipe("Child sending SHIMCMD_STORESTATS().\n");
    write1ByteCmd(SHIMCMD_STORESTATS);
} /* STEAMSHIM_storeStats */

void STEAMSHIM_setAchievement(const char *name, const int enable)
{
    uint8 buf[256];
    uint8 *ptr = buf+1;
    if (isDead()) return;
    dbgpipe("Child sending SHIMCMD_SETACHIEVEMENT('%s', %senable).\n", name, enable ? "" : "!");
    *(ptr++) = (uint8) SHIMCMD_SETACHIEVEMENT;
    *(ptr++) = enable ? 1 : 0;
    strcpy((char *) ptr, name);
    ptr += strlen(name) + 1;
    buf[0] = (uint8) ((ptr-1) - buf);
    writePipe(GPipeWrite, buf, buf[0] + 1);
} /* STEAMSHIM_setAchievement */

void STEAMSHIM_getAchievement(const char *name)
{
    uint8 buf[256];
    uint8 *ptr = buf+1;
    if (isDead()) return;
    dbgpipe("Child sending SHIMCMD_GETACHIEVEMENT('%s').\n", name);
    *(ptr++) = (uint8) SHIMCMD_GETACHIEVEMENT;
    strcpy((char *) ptr, name);
    ptr += strlen(name) + 1;
    buf[0] = (uint8) ((ptr-1) - buf);
    writePipe(GPipeWrite, buf, buf[0] + 1);
} /* STEAMSHIM_getAchievement */

void STEAMSHIM_resetStats(const int bAlsoAchievements)
{
    if (isDead()) return;
    dbgpipe("Child sending SHIMCMD_RESETSTATS(%salsoAchievements).\n", bAlsoAchievements ? "" : "!");
    write2ByteCmd(SHIMCMD_RESETSTATS, bAlsoAchievements ? 1 : 0);
} /* STEAMSHIM_resetStats */

static void writeStatThing(const ShimCmd cmd, const char *name, const void *val, const size_t vallen)
{
    uint8 buf[256];
    uint8 *ptr = buf+1;
    if (isDead()) return;
    *(ptr++) = (uint8) cmd;
    if (vallen)
    {
        memcpy(ptr, val, vallen);
        ptr += vallen;
    } /* if */
    strcpy((char *) ptr, name);
    ptr += strlen(name) + 1;
    buf[0] = (uint8) ((ptr-1) - buf);
    writePipe(GPipeWrite, buf, buf[0] + 1);
} /* writeStatThing */

void STEAMSHIM_setStatI(const char *name, const int _val)
{
    const int32 val = (int32) _val;
    dbgpipe("Child sending SHIMCMD_SETSTATI('%s', val %d).\n", name, val);
    writeStatThing(SHIMCMD_SETSTATI, name, &val, sizeof (val));
} /* STEAMSHIM_setStatI */

void STEAMSHIM_getStatI(const char *name)
{
    dbgpipe("Child sending SHIMCMD_GETSTATI('%s').\n", name);
    writeStatThing(SHIMCMD_GETSTATI, name, NULL, 0);
} /* STEAMSHIM_getStatI */

void STEAMSHIM_setStatF(const char *name, const float val)
{
    dbgpipe("Child sending SHIMCMD_SETSTATF('%s', val %f).\n", name, val);
    writeStatThing(SHIMCMD_SETSTATF, name, &val, sizeof (val));
} /* STEAMSHIM_setStatF */

void STEAMSHIM_getStatF(const char *name)
{
    dbgpipe("Child sending SHIMCMD_GETSTATF('%s').\n", name);
    writeStatThing(SHIMCMD_GETSTATF, name, NULL, 0);
} /* STEAMSHIM_getStatF */

void STEAMSHIM_getSteamID()
{
	write1ByteCmd(SHIMCMD_REQUESTSTEAMID);
}

void STEAMSHIM_getPersonaName(){
    write1ByteCmd(SHIMCMD_REQUESTPERSONANAME);
}

void STEAMSHIM_getAuthSessionTicket(){
    write1ByteCmd(SHIMCMD_REQUESTAUTHSESSIONTICKET);
}
void STEAMSHIM_beginAuthSession(uint64 steamid, SteamAuthTicket_t* ticket){
    printf("stewamid: %llu, %llu, -ticket- \n",steamid,ticket->pcbTicket);
    PIPE_Init();
    PIPE_WriteLong(steamid);
    PIPE_WriteLong(ticket->pcbTicket);
    PIPE_WriteData(ticket->pTicket, AUTH_TICKET_MAXSIZE);
    PIPE_SendCmd(SHIMCMD_BEGINAUTHSESSION);
}

void STEAMSHIM_setRichPresence(const char* key, const char* val){
    uint8 buf[256];
    uint8 *ptr = buf+1;
    *(ptr++) = (uint8) SHIMCMD_SETRICHPRESENCE;
    strcpy((char *) ptr, key);
    ptr += strlen(key) + 1;
    strcpy((char *) ptr, val);
    ptr += strlen(val) + 1;
    buf[0] = (uint8) ((ptr-1) - buf);
    writePipe(GPipeWrite, buf, buf[0] + 1);
}
/* end of steamshim_child.c ... */
