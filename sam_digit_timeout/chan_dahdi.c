/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2006, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief DAHDI for Pseudo TDM
 *
 * \author Mark Spencer <markster@digium.com>
 *
 * Connects to the DAHDI telephony library as well as
 * libpri. Libpri is optional and needed only if you are
 * going to use ISDN connections.
 *
 * You need to install libraries before you attempt to compile
 * and install the DAHDI channel.
 *
 * \par See also
 * \arg \ref Config_dahdi
 *
 * \ingroup channel_drivers
 *
 * \todo Deprecate the "musiconhold" configuration option post 1.4
 */

/*** MODULEINFO
	<depend>res_smdi</depend>
	<depend>dahdi</depend>
	<depend>tonezone</depend>
	<use>pri</use>
	<use>ss7</use>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision: 139400 $")

#ifdef __NetBSD__
#include <pthread.h>
#include <signal.h>
#else
#include <sys/signal.h>
#endif
#include <sys/ioctl.h>
#include <math.h>
#include <ctype.h>

#include <dahdi/user.h>
#include <dahdi/tonezone.h>

#ifdef HAVE_PRI
#include <libpri.h>
#endif

#ifdef HAVE_SS7
#include <libss7.h>
#endif

#include "asterisk/lock.h"
#include "asterisk/channel.h"
#include "asterisk/config.h"
#include "asterisk/module.h"
#include "asterisk/pbx.h"
#include "asterisk/file.h"
#include "asterisk/ulaw.h"
#include "asterisk/alaw.h"
#include "asterisk/callerid.h"
#include "asterisk/adsi.h"
#include "asterisk/cli.h"
#include "asterisk/cdr.h"
#include "asterisk/features.h"
#include "asterisk/musiconhold.h"
#include "asterisk/say.h"
#include "asterisk/tdd.h"
#include "asterisk/app.h"
#include "asterisk/dsp.h"
#include "asterisk/astdb.h"
#include "asterisk/manager.h"
#include "asterisk/causes.h"
#include "asterisk/term.h"
#include "asterisk/utils.h"
#include "asterisk/transcap.h"
#include "asterisk/stringfields.h"
#include "asterisk/abstract_jb.h"
#include "asterisk/smdi.h"
#include "asterisk/astobj.h"
#include "asterisk/event.h"
#include "asterisk/devicestate.h"

#define SMDI_MD_WAIT_TIMEOUT 1500 /* 1.5 seconds */

static const char *lbostr[] = {
"0 db (CSU)/0-133 feet (DSX-1)",
"133-266 feet (DSX-1)",
"266-399 feet (DSX-1)",
"399-533 feet (DSX-1)",
"533-655 feet (DSX-1)",
"-7.5db (CSU)",
"-15db (CSU)",
"-22.5db (CSU)"
};

/*! Global jitterbuffer configuration - by default, jb is disabled */
static struct ast_jb_conf default_jbconf =
{
	.flags = 0,
	.max_size = -1,
	.resync_threshold = -1,
	.impl = ""
};
static struct ast_jb_conf global_jbconf;

/* define this to send PRI user-user information elements */
#undef SUPPORT_USERUSER

/*!
 * \note Define ZHONE_HACK to cause us to go off hook and then back on hook when
 * the user hangs up to reset the state machine so ring works properly.
 * This is used to be able to support kewlstart by putting the zhone in
 * groundstart mode since their forward disconnect supervision is entirely
 * broken even though their documentation says it isn't and their support
 * is entirely unwilling to provide any assistance with their channel banks
 * even though their web site says they support their products for life.
 */
/* #define ZHONE_HACK */

/*! \note
 * Define if you want to check the hook state for an FXO (FXS signalled) interface
 * before dialing on it.  Certain FXO interfaces always think they're out of
 * service with this method however.
 */
/* #define DAHDI_CHECK_HOOKSTATE */

/*! \brief Typically, how many rings before we should send Caller*ID */
#define DEFAULT_CIDRINGS 1

#define CHANNEL_PSEUDO -12

#define AST_LAW(p) (((p)->law == DAHDI_LAW_ALAW) ? AST_FORMAT_ALAW : AST_FORMAT_ULAW)


/*! \brief Signaling types that need to use MF detection should be placed in this macro */
#define NEED_MFDETECT(p) (((p)->sig == SIG_FEATDMF) || ((p)->sig == SIG_FEATDMF_TA) || ((p)->sig == SIG_E911) || ((p)->sig == SIG_FGC_CAMA) || ((p)->sig == SIG_FGC_CAMAMF) || ((p)->sig == SIG_FEATB))

static const char tdesc[] = "DAHDI Telephony Driver"
#ifdef HAVE_PRI
               " w/PRI"
#endif
#ifdef HAVE_SS7
	       " w/SS7"
#endif
;

static const char config[] = "chan_dahdi.conf";

#define SIG_EM		DAHDI_SIG_EM
#define SIG_EMWINK 	(0x0100000 | DAHDI_SIG_EM)
#define SIG_FEATD	(0x0200000 | DAHDI_SIG_EM)
#define	SIG_FEATDMF	(0x0400000 | DAHDI_SIG_EM)
#define	SIG_FEATB	(0x0800000 | DAHDI_SIG_EM)
#define	SIG_E911	(0x1000000 | DAHDI_SIG_EM)
#define	SIG_FEATDMF_TA	(0x2000000 | DAHDI_SIG_EM)
#define	SIG_FGC_CAMA	(0x4000000 | DAHDI_SIG_EM)
#define	SIG_FGC_CAMAMF	(0x8000000 | DAHDI_SIG_EM)
#define SIG_FXSLS	DAHDI_SIG_FXSLS
#define SIG_FXSGS	DAHDI_SIG_FXSGS
#define SIG_FXSKS	DAHDI_SIG_FXSKS
#define SIG_FXOLS	DAHDI_SIG_FXOLS
#define SIG_FXOGS	DAHDI_SIG_FXOGS
#define SIG_FXOKS	DAHDI_SIG_FXOKS
#define SIG_PRI		DAHDI_SIG_CLEAR
#define SIG_BRI		(0x2000000 | DAHDI_SIG_CLEAR)
#define SIG_BRI_PTMP	(0X4000000 | DAHDI_SIG_CLEAR)
#define SIG_SS7		(0x1000000 | DAHDI_SIG_CLEAR)
#define	SIG_SF		DAHDI_SIG_SF
#define SIG_SFWINK 	(0x0100000 | DAHDI_SIG_SF)
#define SIG_SF_FEATD	(0x0200000 | DAHDI_SIG_SF)
#define	SIG_SF_FEATDMF	(0x0400000 | DAHDI_SIG_SF)
#define	SIG_SF_FEATB	(0x0800000 | DAHDI_SIG_SF)
#define SIG_EM_E1	DAHDI_SIG_EM_E1
#define SIG_GR303FXOKS	(0x0100000 | DAHDI_SIG_FXOKS)
#define SIG_GR303FXSKS	(0x0100000 | DAHDI_SIG_FXSKS)

#ifdef LOTS_OF_SPANS
#define NUM_SPANS	DAHDI_MAX_SPANS
#else
#define NUM_SPANS 		32
#endif
#define NUM_DCHANS		4	/*!< No more than 4 d-channels */
#define MAX_CHANNELS	672		/*!< No more than a DS3 per trunk group */

#define CHAN_PSEUDO	-2

#define DCHAN_PROVISIONED (1 << 0)
#define DCHAN_NOTINALARM  (1 << 1)
#define DCHAN_UP          (1 << 2)

#define DCHAN_AVAILABLE	(DCHAN_PROVISIONED | DCHAN_NOTINALARM | DCHAN_UP)

/* Overlap dialing option types */
#define DAHDI_OVERLAPDIAL_NONE 0
#define DAHDI_OVERLAPDIAL_OUTGOING 1
#define DAHDI_OVERLAPDIAL_INCOMING 2
#define DAHDI_OVERLAPDIAL_BOTH (DAHDI_OVERLAPDIAL_INCOMING|DAHDI_OVERLAPDIAL_OUTGOING)


#define CALLPROGRESS_PROGRESS		1
#define CALLPROGRESS_FAX_OUTGOING	2
#define CALLPROGRESS_FAX_INCOMING	4
#define CALLPROGRESS_FAX		(CALLPROGRESS_FAX_INCOMING | CALLPROGRESS_FAX_OUTGOING)

static char defaultcic[64] = "";
static char defaultozz[64] = "";

/*! Run this script when the MWI state changes on an FXO line, if mwimonitor is enabled */
static char mwimonitornotify[PATH_MAX] = "";

static char progzone[10] = "";

static int usedistinctiveringdetection = 0;
static int distinctiveringaftercid = 0;

static int numbufs = 4;

static int mwilevel = 512;

#ifdef HAVE_PRI
static struct ast_channel inuse;
#ifdef PRI_GETSET_TIMERS
static int pritimers[PRI_MAX_TIMERS];
#endif
static int pridebugfd = -1;
static char pridebugfilename[1024] = "";
#endif

/*! \brief Wait up to 16 seconds for first digit (FXO logic) */
static int firstdigittimeout = 16000;

/*! \brief How long to wait for following digits (FXO logic) */
static int gendigittimeout = 8000;

/*! \brief How long to wait for an extra digit, if there is an ambiguous match */
static int matchdigittimeout = 3000;

/*! \brief Protect the interface list (of dahdi_pvt's) */
AST_MUTEX_DEFINE_STATIC(iflock);


static int ifcount = 0;

#ifdef HAVE_PRI
AST_MUTEX_DEFINE_STATIC(pridebugfdlock);
#endif

/*! \brief Protect the monitoring thread, so only one process can kill or start it, and not
   when it's doing something critical. */
AST_MUTEX_DEFINE_STATIC(monlock);

/*! \brief This is the thread for the monitor which checks for input on the channels
   which are not currently in use. */
static pthread_t monitor_thread = AST_PTHREADT_NULL;
static ast_cond_t ss_thread_complete;
AST_MUTEX_DEFINE_STATIC(ss_thread_lock);
AST_MUTEX_DEFINE_STATIC(restart_lock);
static int ss_thread_count = 0;
static int num_restart_pending = 0;

static int restart_monitor(void);

static enum ast_bridge_result dahdi_bridge(struct ast_channel *c0, struct ast_channel *c1, int flags, struct ast_frame **fo, struct ast_channel **rc, int timeoutms);

static int dahdi_sendtext(struct ast_channel *c, const char *text);

static void mwi_event_cb(const struct ast_event *event, void *userdata)
{
	/* This module does not handle MWI in an event-based manner.  However, it
	 * subscribes to MWI for each mailbox that is configured so that the core
	 * knows that we care about it.  Then, chan_dahdi will get the MWI from the
	 * event cache instead of checking the mailbox directly. */
}

/*! \brief Avoid the silly dahdi_getevent which ignores a bunch of events */
static inline int dahdi_get_event(int fd)
{
	int j;
	if (ioctl(fd, DAHDI_GETEVENT, &j) == -1)
		return -1;
	return j;
}

/*! \brief Avoid the silly dahdi_waitevent which ignores a bunch of events */
static inline int dahdi_wait_event(int fd)
{
	int i, j = 0;
	i = DAHDI_IOMUX_SIGEVENT;
	if (ioctl(fd, DAHDI_IOMUX, &i) == -1)
		return -1;
	if (ioctl(fd, DAHDI_GETEVENT, &j) == -1)
		return -1;
	return j;
}

/*! Chunk size to read -- we use 20ms chunks to make things happy. */
#define READ_SIZE 160

#define MASK_AVAIL		(1 << 0)	/*!< Channel available for PRI use */
#define MASK_INUSE		(1 << 1)	/*!< Channel currently in use */

#define CALLWAITING_SILENT_SAMPLES	( (300 * 8) / READ_SIZE) /*!< 300 ms */
#define CALLWAITING_REPEAT_SAMPLES	( (10000 * 8) / READ_SIZE) /*!< 10,000 ms */
#define CIDCW_EXPIRE_SAMPLES		( (500 * 8) / READ_SIZE) /*!< 500 ms */
#define MIN_MS_SINCE_FLASH			( (2000) )	/*!< 2000 ms */
#define DEFAULT_RINGT 				( (8000 * 8) / READ_SIZE) /*!< 8,000 ms */

struct dahdi_pvt;

static int ringt_base = DEFAULT_RINGT;

#ifdef HAVE_SS7

#define LINKSTATE_INALARM	(1 << 0)
#define LINKSTATE_STARTING	(1 << 1)
#define LINKSTATE_UP		(1 << 2)
#define LINKSTATE_DOWN		(1 << 3)

#define SS7_NAI_DYNAMIC		-1

#define LINKSET_FLAG_EXPLICITACM (1 << 0)
#define LINKSET_FLAG_INITIALHWBLO (1 << 1)
#define LINKSET_FLAG_USEECHOCONTROL (1 << 2)
#define LINKSET_FLAG_DEFAULTECHOCONTROL (1 << 3)

#define SS7_BLOCKED_HARDWARE 1 << 1
#define SS7_BLOCKED_MAINTENANCE 1 << 0

struct dahdi_ss7 {
	pthread_t master;						/*!< Thread of master */
	ast_mutex_t lock;
	int fds[NUM_DCHANS];
	int numsigchans;
	int linkstate[NUM_DCHANS];
	int numchans;
	int type;
	enum {
		LINKSET_STATE_DOWN = 0,
		LINKSET_STATE_UP
	} state;
	char called_nai;						/*!< Called Nature of Address Indicator */
	char calling_nai;						/*!< Calling Nature of Address Indicator */
	char internationalprefix[10];					/*!< country access code ('00' for european dialplans) */
	char nationalprefix[10];					/*!< area access code ('0' for european dialplans) */
	char subscriberprefix[20];					/*!< area access code + area code ('0'+area code for european dialplans) */
	char unknownprefix[20];						/*!< for unknown dialplans */
	char networkroutedprefix[20];
	struct ss7 *ss7;
	struct dahdi_pvt *pvts[MAX_CHANNELS];				/*!< Member channel pvt structs */
	int flags;							/*!< Linkset flags */
};

static struct dahdi_ss7 linksets[NUM_SPANS];

static int cur_ss7type = -1;
static int cur_linkset = -1;
static int cur_pointcode = -1;
static int cur_cicbeginswith = -1;
static int cur_adjpointcode = -1;
static int cur_networkindicator = -1;
static int cur_defaultdpc = -1;
#endif /* HAVE_SS7 */

#ifdef HAVE_PRI

#define PVT_TO_CHANNEL(p) (((p)->prioffset) | ((p)->logicalspan << 8) | (p->pri->mastertrunkgroup ? 0x10000 : 0))
#define PRI_CHANNEL(p) ((p) & 0xff)
#define PRI_SPAN(p) (((p) >> 8) & 0xff)
#define PRI_EXPLICIT(p) (((p) >> 16) & 0x01)

struct dahdi_pri {
	pthread_t master;						/*!< Thread of master */
	ast_mutex_t lock;						/*!< Mutex */
	char idleext[AST_MAX_EXTENSION];				/*!< Where to idle extra calls */
	char idlecontext[AST_MAX_CONTEXT];				/*!< What context to use for idle */
	char idledial[AST_MAX_EXTENSION];				/*!< What to dial before dumping */
	int minunused;							/*!< Min # of channels to keep empty */
	int minidle;							/*!< Min # of "idling" calls to keep active */
	int nodetype;							/*!< Node type */
	int switchtype;							/*!< Type of switch to emulate */
	int nsf;							/*!< Network-Specific Facilities */
	int dialplan;							/*!< Dialing plan */
	int localdialplan;						/*!< Local dialing plan */
	char internationalprefix[10];					/*!< country access code ('00' for european dialplans) */
	char nationalprefix[10];					/*!< area access code ('0' for european dialplans) */
	char localprefix[20];						/*!< area access code + area code ('0'+area code for european dialplans) */
	char privateprefix[20];						/*!< for private dialplans */
	char unknownprefix[20];						/*!< for unknown dialplans */
	int dchannels[NUM_DCHANS];					/*!< What channel are the dchannels on */
	int trunkgroup;							/*!< What our trunkgroup is */
	int mastertrunkgroup;						/*!< What trunk group is our master */
	int prilogicalspan;						/*!< Logical span number within trunk group */
	int numchans;							/*!< Num of channels we represent */
	int overlapdial;						/*!< In overlap dialing mode */
	int facilityenable;						/*!< Enable facility IEs */
	struct pri *dchans[NUM_DCHANS];					/*!< Actual d-channels */
	int dchanavail[NUM_DCHANS];					/*!< Whether each channel is available */
	struct pri *pri;						/*!< Currently active D-channel */
	int debug;
	int fds[NUM_DCHANS];						/*!< FD's for d-channels */
	int offset;
	int span;
	int resetting;
	int resetpos;
#ifdef HAVE_PRI_INBANDDISCONNECT
	unsigned int inbanddisconnect:1;				/*!< Should we support inband audio after receiving DISCONNECT? */
#endif
	time_t lastreset;						/*!< time when unused channels were last reset */
	long resetinterval;						/*!< Interval (in seconds) for resetting unused channels */
	int sig;
	struct dahdi_pvt *pvts[MAX_CHANNELS];				/*!< Member channel pvt structs */
	struct dahdi_pvt *crvs;						/*!< Member CRV structs */
	struct dahdi_pvt *crvend;						/*!< Pointer to end of CRV structs */
};


static struct dahdi_pri pris[NUM_SPANS];

#if 0
#define DEFAULT_PRI_DEBUG (PRI_DEBUG_Q931_DUMP | PRI_DEBUG_Q921_DUMP | PRI_DEBUG_Q921_RAW | PRI_DEBUG_Q921_STATE)
#else
#define DEFAULT_PRI_DEBUG 0
#endif

static inline void pri_rel(struct dahdi_pri *pri)
{
	ast_mutex_unlock(&pri->lock);
}

#else
/*! Shut up the compiler */
struct dahdi_pri;
#endif

#define SUB_REAL	0			/*!< Active call */
#define SUB_CALLWAIT	1			/*!< Call-Waiting call on hold */
#define SUB_THREEWAY	2			/*!< Three-way call */

/* Polarity states */
#define POLARITY_IDLE   0
#define POLARITY_REV    1


struct distRingData {
	int ring[3];
	int range;
};
struct ringContextData {
	char contextData[AST_MAX_CONTEXT];
};
struct dahdi_distRings {
	struct distRingData ringnum[3];
	struct ringContextData ringContext[3];
};

static char *subnames[] = {
	"Real",
	"Callwait",
	"Threeway"
};

struct dahdi_subchannel {
	int dfd;
	struct ast_channel *owner;
	int chan;
	short buffer[AST_FRIENDLY_OFFSET/2 + READ_SIZE];
	struct ast_frame f;		/*!< One frame for each channel.  How did this ever work before? */
	unsigned int needringing:1;
	unsigned int needbusy:1;
	unsigned int needcongestion:1;
	unsigned int needcallerid:1;
	unsigned int needanswer:1;
	unsigned int needflash:1;
	unsigned int needhold:1;
	unsigned int needunhold:1;
	unsigned int linear:1;
	unsigned int inthreeway:1;
	struct dahdi_confinfo curconf;
};

#define CONF_USER_REAL		(1 << 0)
#define CONF_USER_THIRDCALL	(1 << 1)

#define MAX_SLAVES	4

static struct dahdi_pvt {
	ast_mutex_t lock;
	struct ast_channel *owner;			/*!< Our current active owner (if applicable) */
							/*!< Up to three channels can be associated with this call */

	struct dahdi_subchannel sub_unused;		/*!< Just a safety precaution */
	struct dahdi_subchannel subs[3];			/*!< Sub-channels */
	struct dahdi_confinfo saveconf;			/*!< Saved conference info */

	struct dahdi_pvt *slaves[MAX_SLAVES];		/*!< Slave to us (follows our conferencing) */
	struct dahdi_pvt *master;				/*!< Master to us (we follow their conferencing) */
	int inconference;				/*!< If our real should be in the conference */

	int buf_no;					/*!< Number of buffers */
	int buf_policy;				/*!< Buffer policy */
	int sig;					/*!< Signalling style */
	int radio;					/*!< radio type */
	int outsigmod;					/*!< Outbound Signalling style (modifier) */
	int oprmode;					/*!< "Operator Services" mode */
	struct dahdi_pvt *oprpeer;				/*!< "Operator Services" peer tech_pvt ptr */
	float cid_rxgain;					/*!< "Gain to apply during caller id */
	float rxgain;
	float txgain;
	int tonezone;					/*!< tone zone for this chan, or -1 for default */
	struct dahdi_pvt *next;				/*!< Next channel in list */
	struct dahdi_pvt *prev;				/*!< Prev channel in list */

	/* flags */
	unsigned int adsi:1;
	unsigned int answeronpolarityswitch:1;
	unsigned int busydetect:1;
	unsigned int callreturn:1;
	unsigned int callwaiting:1;
	unsigned int callwaitingcallerid:1;
	unsigned int cancallforward:1;
	unsigned int canpark:1;
	unsigned int confirmanswer:1;			/*!< Wait for '#' to confirm answer */
	unsigned int destroy:1;
	unsigned int didtdd:1;				/*!< flag to say its done it once */
	unsigned int dialednone:1;
	unsigned int dialing:1;
	unsigned int digital:1;
	unsigned int dnd:1;
	unsigned int echobreak:1;
	unsigned int echocanbridged:1;
	unsigned int echocanon:1;
	unsigned int faxhandled:1;			/*!< Has a fax tone already been handled? */
	unsigned int firstradio:1;
	unsigned int hanguponpolarityswitch:1;
	unsigned int hardwaredtmf:1;
	unsigned int hidecallerid:1;
	unsigned int hidecalleridname:1;      /*!< Hide just the name not the number for legacy PBX use */
	unsigned int ignoredtmf:1;
	unsigned int immediate:1;			/*!< Answer before getting digits? */
	unsigned int inalarm:1;
	unsigned int mate:1;				/*!< flag to say its in MATE mode */
	unsigned int outgoing:1;
	/* unsigned int overlapdial:1; 			unused and potentially confusing */
	unsigned int permcallwaiting:1;
	unsigned int permhidecallerid:1;		/*!< Whether to hide our outgoing caller ID or not */
	unsigned int priindication_oob:1;
	unsigned int priexclusive:1;
	unsigned int pulse:1;
	unsigned int pulsedial:1;			/*!< whether a pulse dial phone is detected */
	unsigned int restartpending:1;		/*!< flag to ensure counted only once for restart */
	unsigned int restrictcid:1;			/*!< Whether restrict the callerid -> only send ANI */
	unsigned int threewaycalling:1;
	unsigned int transfer:1;
	unsigned int use_callerid:1;			/*!< Whether or not to use caller id on this channel */
	unsigned int use_callingpres:1;			/*!< Whether to use the callingpres the calling switch sends */
	unsigned int usedistinctiveringdetection:1;
	unsigned int dahditrcallerid:1;			/*!< should we use the callerid from incoming call on dahdi transfer or not */
	unsigned int transfertobusy:1;			/*!< allow flash-transfers to busy channels */
	unsigned int mwimonitor_neon:1;			/*!< monitor this FXO port for neon type MWI indication from other end */
	unsigned int mwimonitor_fsk:1;			/*!< monitor this FXO port for fsk MWI indication from other end */
	unsigned int mwimonitoractive:1;		/*!< an MWI monitor thread is currently active */
	/* Channel state or unavilability flags */
	unsigned int inservice:1;
	unsigned int locallyblocked:2;
	unsigned int remotelyblocked:2;
#if defined(HAVE_PRI) || defined(HAVE_SS7)
	unsigned int rlt:1;
	unsigned int alerting:1;
	unsigned int alreadyhungup:1;
	unsigned int isidlecall:1;
	unsigned int proceeding:1;
	unsigned int progress:1;
	unsigned int resetting:1;
	unsigned int setup_ack:1;
#endif
#if HAVE_SS7
	unsigned int do_hangup; /* what have to do in chan_dahdi */
	unsigned int called_complete;
	unsigned int echocontrol_ind;
#endif
	unsigned int use_smdi:1;		/* Whether to use SMDI on this channel */
	struct ast_smdi_interface *smdi_iface;	/* The serial port to listen for SMDI data on */

	struct dahdi_distRings drings;

	char context[AST_MAX_CONTEXT];
	char defcontext[AST_MAX_CONTEXT];
	char exten[AST_MAX_EXTENSION];
	char language[MAX_LANGUAGE];
	char mohinterpret[MAX_MUSICCLASS];
	char mohsuggest[MAX_MUSICCLASS];
#if defined(PRI_ANI) || defined(HAVE_SS7)
	char cid_ani[AST_MAX_EXTENSION];
#endif
	int cid_ani2;
	char cid_num[AST_MAX_EXTENSION];
	int cid_ton;					/*!< Type Of Number (TON) */
	char cid_name[AST_MAX_EXTENSION];
	char lastcid_num[AST_MAX_EXTENSION];
	char lastcid_name[AST_MAX_EXTENSION];
	char *origcid_num;				/*!< malloced original callerid */
	char *origcid_name;				/*!< malloced original callerid */
	char callwait_num[AST_MAX_EXTENSION];
	char callwait_name[AST_MAX_EXTENSION];
	char rdnis[AST_MAX_EXTENSION];
	char dnid[AST_MAX_EXTENSION];
	ast_group_t group;
	int law;
	int confno;					/*!< Our conference */
	int confusers;					/*!< Who is using our conference */
	int propconfno;					/*!< Propagated conference number */
	ast_group_t callgroup;
	ast_group_t pickupgroup;
	struct ast_variable *vars;
	int channel;					/*!< Channel Number or CRV */
	int span;					/*!< Span number */
	time_t guardtime;				/*!< Must wait this much time before using for new call */
	int cid_signalling;				/*!< CID signalling type bell202 or v23 */
	int cid_start;					/*!< CID start indicator, polarity or ring */
	int callingpres;				/*!< The value of callling presentation that we're going to use when placing a PRI call */
	int callwaitingrepeat;				/*!< How many samples to wait before repeating call waiting */
	int cidcwexpire;				/*!< When to expire our muting for CID/CW */
	unsigned char *cidspill;
	int cidpos;
	int cidlen;
	int ringt;
	int ringt_base;
	int stripmsd;
	int callwaitcas;
	int callwaitrings;
	struct {
		struct dahdi_echocanparams head;
		struct dahdi_echocanparam params[DAHDI_MAX_ECHOCANPARAMS];
	} echocancel;
	int echotraining;
	char echorest[20];
	int busycount;
	int busy_tonelength;
	int busy_quietlength;
	int callprogress;
	struct timeval flashtime;			/*!< Last flash-hook time */
	struct ast_dsp *dsp;
	int cref;					/*!< Call reference number */
	struct dahdi_dialoperation dop;
	int whichwink;					/*!< SIG_FEATDMF_TA Which wink are we on? */
	char finaldial[64];
	char accountcode[AST_MAX_ACCOUNT_CODE];		/*!< Account code */
	int amaflags;					/*!< AMA Flags */
	struct tdd_state *tdd;				/*!< TDD flag */
	char call_forward[AST_MAX_EXTENSION];
	char mailbox[AST_MAX_EXTENSION];
	struct ast_event_sub *mwi_event_sub;
	char dialdest[256];
	int onhooktime;
	int msgstate;
	int distinctivering;				/*!< Which distinctivering to use */
	int cidrings;					/*!< Which ring to deliver CID on */
	int dtmfrelax;					/*!< whether to run in relaxed DTMF mode */
	int fake_event;
	int polarityonanswerdelay;
	struct timeval polaritydelaytv;
	int sendcalleridafter;
#ifdef HAVE_PRI
	struct dahdi_pri *pri;
	struct dahdi_pvt *bearer;
	struct dahdi_pvt *realcall;
	q931_call *call;
	int prioffset;
	int logicalspan;
#endif
	int polarity;
	int dsp_features;
#ifdef HAVE_SS7
	struct dahdi_ss7 *ss7;
	struct isup_call *ss7call;
	char charge_number[50];
	char gen_add_number[50];
	char gen_dig_number[50];
	char orig_called_num[50];
	char redirecting_num[50];
	unsigned char redirect_counter;
	unsigned char redirect_info;
	unsigned char redirect_info_ind;
	unsigned char redirect_info_orig_reas;
	unsigned char redirect_info_counter;
	unsigned char redirect_info_reas;
	char generic_name[50];
	unsigned char gen_add_num_plan;
	unsigned char gen_add_nai;
	unsigned char gen_add_pres_ind;
	unsigned char gen_add_type;
	unsigned char gen_dig_type;
	unsigned char gen_dig_scheme;
	char jip_number[50];
	unsigned char lspi_type;
	unsigned char lspi_scheme;
	unsigned char lspi_context;
	char lspi_ident[50];
	unsigned int call_ref_ident;
	unsigned int call_ref_pc;
	unsigned char calling_party_cat;
	int transcap;
	int cic;							/*!< CIC associated with channel */
	unsigned int dpc;						/*!< CIC's DPC */
	unsigned int loopedback:1;
	char cug_interlock_ni[5];
	unsigned short cug_interlock_code;
	unsigned char cug_indicator;
#endif
	char begindigit;
} *iflist = NULL, *ifend = NULL;

/*! \brief Channel configuration from chan_dahdi.conf .
 * This struct is used for parsing the [channels] section of chan_dahdi.conf.
 * Generally there is a field here for every possible configuration item.
 *
 * The state of fields is saved along the parsing and whenever a 'channel'
 * statement is reached, the current dahdi_chan_conf is used to configure the
 * channel (struct dahdi_pvt)
 *
 * \see dahdi_chan_init for the default values.
 */
struct dahdi_chan_conf {
	struct dahdi_pvt chan;
#ifdef HAVE_PRI
	struct dahdi_pri pri;
#endif

#ifdef HAVE_SS7
	struct dahdi_ss7 ss7;
#endif
	struct dahdi_params timing;
	int is_sig_auto; /*!< Use channel signalling from DAHDI? */

	char smdi_port[SMDI_MAX_FILENAME_LEN];
};

/*! returns a new dahdi_chan_conf with default values (by-value) */
static struct dahdi_chan_conf dahdi_chan_conf_default(void) {
	/* recall that if a field is not included here it is initialized
	 * to 0 or equivalent
	 */
	struct dahdi_chan_conf conf = {
#ifdef HAVE_PRI
		.pri = {
			.nsf = PRI_NSF_NONE,
			.switchtype = PRI_SWITCH_NI2,
			.dialplan = PRI_NATIONAL_ISDN + 1,
			.localdialplan = PRI_NATIONAL_ISDN + 1,
			.nodetype = PRI_CPE,

			.minunused = 2,
			.idleext = "",
			.idledial = "",
			.internationalprefix = "",
			.nationalprefix = "",
			.localprefix = "",
			.privateprefix = "",
			.unknownprefix = "",
			.resetinterval = -1,
		},
#endif
#ifdef HAVE_SS7
		.ss7 = {
			.called_nai = SS7_NAI_NATIONAL,
			.calling_nai = SS7_NAI_NATIONAL,
			.internationalprefix = "",
			.nationalprefix = "",
			.subscriberprefix = "",
			.unknownprefix = "",
			.networkroutedprefix = "",
		},
#endif
		.chan = {
			.context = "default",
			.cid_num = "",
			.cid_name = "",
			.mohinterpret = "default",
			.mohsuggest = "",
			.transfertobusy = 1,

			.cid_signalling = CID_SIG_BELL,
			.cid_start = CID_START_RING,
			.dahditrcallerid = 0,
			.use_callerid = 1,
			.sig = -1,
			.outsigmod = -1,

			.cid_rxgain = +5.0,

			.tonezone = -1,

			.echocancel.head.tap_length = 1,

			.busycount = 3,

			.accountcode = "",

			.mailbox = "",


			.polarityonanswerdelay = 600,

			.sendcalleridafter = DEFAULT_CIDRINGS,

			.buf_policy = DAHDI_POLICY_IMMEDIATE,
			.buf_no = numbufs
		},
		.timing = {
			.prewinktime = -1,
			.preflashtime = -1,
			.winktime = -1,
			.flashtime = -1,
			.starttime = -1,
			.rxwinktime = -1,
			.rxflashtime = -1,
			.debouncetime = -1
		},
		.is_sig_auto = 1,
		.smdi_port = "/dev/ttyS0",
	};

	return conf;
}


static struct ast_channel *dahdi_request(const char *type, int format, void *data, int *cause);
static int dahdi_digit_begin(struct ast_channel *ast, char digit);
static int dahdi_digit_end(struct ast_channel *ast, char digit, unsigned int duration);
static int dahdi_sendtext(struct ast_channel *c, const char *text);
static int dahdi_call(struct ast_channel *ast, char *rdest, int timeout);
static int dahdi_hangup(struct ast_channel *ast);
static int dahdi_answer(struct ast_channel *ast);
static struct ast_frame *dahdi_read(struct ast_channel *ast);
static int dahdi_write(struct ast_channel *ast, struct ast_frame *frame);
static struct ast_frame *dahdi_exception(struct ast_channel *ast);
static int dahdi_indicate(struct ast_channel *chan, int condition, const void *data, size_t datalen);
static int dahdi_fixup(struct ast_channel *oldchan, struct ast_channel *newchan);
static int dahdi_setoption(struct ast_channel *chan, int option, void *data, int datalen);
static int dahdi_func_read(struct ast_channel *chan, const char *function, char *data, char *buf, size_t len);

static const struct ast_channel_tech dahdi_tech = {
	.type = "DAHDI",
	.description = tdesc,
	.capabilities = AST_FORMAT_SLINEAR | AST_FORMAT_ULAW | AST_FORMAT_ALAW,
	.requester = dahdi_request,
	.send_digit_begin = dahdi_digit_begin,
	.send_digit_end = dahdi_digit_end,
	.send_text = dahdi_sendtext,
	.call = dahdi_call,
	.hangup = dahdi_hangup,
	.answer = dahdi_answer,
	.read = dahdi_read,
	.write = dahdi_write,
	.bridge = dahdi_bridge,
	.exception = dahdi_exception,
	.indicate = dahdi_indicate,
	.fixup = dahdi_fixup,
	.setoption = dahdi_setoption,
	.func_channel_read = dahdi_func_read,
};

#ifdef HAVE_PRI
#define GET_CHANNEL(p) ((p)->bearer ? (p)->bearer->channel : p->channel)
#else
#define GET_CHANNEL(p) ((p)->channel)
#endif

struct dahdi_pvt *round_robin[32];

#ifdef HAVE_PRI
static inline int pri_grab(struct dahdi_pvt *pvt, struct dahdi_pri *pri)
{
	int res;
	/* Grab the lock first */
	do {
		res = ast_mutex_trylock(&pri->lock);
		if (res) {
			DEADLOCK_AVOIDANCE(&pvt->lock);
		}
	} while (res);
	/* Then break the poll */
	if (pri->master != AST_PTHREADT_NULL)
		pthread_kill(pri->master, SIGURG);
	return 0;
}
#endif

#ifdef HAVE_SS7
static int ss7_find_alloc_call(struct dahdi_pvt *p);
static int ss7_do_rsc(struct dahdi_pvt *p);

static inline void ss7_rel(struct dahdi_ss7 *ss7)
{
	ast_mutex_unlock(&ss7->lock);
}

static inline int ss7_grab(struct dahdi_pvt *pvt, struct dahdi_ss7 *pri)
{
	int res;
	/* Grab the lock first */
	do {
		res = ast_mutex_trylock(&pri->lock);
		if (res) {
			ast_mutex_unlock(&pvt->lock);
			/* Release the lock and try again */
			usleep(1);
			ast_mutex_lock(&pvt->lock);
		}
	} while (res);
	/* Then break the poll */
	if (pri->master != AST_PTHREADT_NULL)
		pthread_kill(pri->master, SIGURG);
	return 0;
}
#endif
#define NUM_CADENCE_MAX 25
static int num_cadence = 4;
static int user_has_defined_cadences = 0;

static struct dahdi_ring_cadence cadences[NUM_CADENCE_MAX] = {
	{ { 125, 125, 2000, 4000 } },			/*!< Quick chirp followed by normal ring */
	{ { 250, 250, 500, 1000, 250, 250, 500, 4000 } }, /*!< British style ring */
	{ { 125, 125, 125, 125, 125, 4000 } },	/*!< Three short bursts */
	{ { 1000, 500, 2500, 5000 } },	/*!< Long ring */
};

/*! \brief cidrings says in which pause to transmit the cid information, where the first pause
 * is 1, the second pause is 2 and so on.
 */

static int cidrings[NUM_CADENCE_MAX] = {
	2,										/*!< Right after first long ring */
	4,										/*!< Right after long part */
	3,										/*!< After third chirp */
	2,										/*!< Second spell */
};

#define ISTRUNK(p) ((p->sig == SIG_FXSLS) || (p->sig == SIG_FXSKS) || \
			(p->sig == SIG_FXSGS) || (p->sig == SIG_PRI))

#define CANBUSYDETECT(p) (ISTRUNK(p) || (p->sig & (SIG_EM | SIG_EM_E1 | SIG_SF)) /* || (p->sig & __DAHDI_SIG_FXO) */)
#define CANPROGRESSDETECT(p) (ISTRUNK(p) || (p->sig & (SIG_EM | SIG_EM_E1 | SIG_SF)) /* || (p->sig & __DAHDI_SIG_FXO) */)

static int dahdi_get_index(struct ast_channel *ast, struct dahdi_pvt *p, int nullok)
{
	int res;
	if (p->subs[SUB_REAL].owner == ast)
		res = 0;
	else if (p->subs[SUB_CALLWAIT].owner == ast)
		res = 1;
	else if (p->subs[SUB_THREEWAY].owner == ast)
		res = 2;
	else {
		res = -1;
		if (!nullok)
			ast_log(LOG_WARNING, "Unable to get index, and nullok is not asserted\n");
	}
	return res;
}

#ifdef HAVE_PRI
static void wakeup_sub(struct dahdi_pvt *p, int a, struct dahdi_pri *pri)
#else
static void wakeup_sub(struct dahdi_pvt *p, int a, void *pri)
#endif
{
#ifdef HAVE_PRI
	if (pri)
		ast_mutex_unlock(&pri->lock);
#endif
	for (;;) {
		if (p->subs[a].owner) {
			if (ast_channel_trylock(p->subs[a].owner)) {
				DEADLOCK_AVOIDANCE(&p->lock);
			} else {
				ast_queue_frame(p->subs[a].owner, &ast_null_frame);
				ast_channel_unlock(p->subs[a].owner);
				break;
			}
		} else
			break;
	}
#ifdef HAVE_PRI
	if (pri)
		ast_mutex_lock(&pri->lock);
#endif
}

static void dahdi_queue_frame(struct dahdi_pvt *p, struct ast_frame *f, void *data)
{
#ifdef HAVE_PRI
	struct dahdi_pri *pri = (struct dahdi_pri*) data;
#endif
#ifdef HAVE_SS7
	struct dahdi_ss7 *ss7 = (struct dahdi_ss7*) data;
#endif
	/* We must unlock the PRI to avoid the possibility of a deadlock */
#if defined(HAVE_PRI) || defined(HAVE_SS7)
	if (data) {
		switch (p->sig) {
#ifdef HAVE_PRI
		case SIG_BRI:
		case SIG_BRI_PTMP:
		case SIG_PRI:
			ast_mutex_unlock(&pri->lock);
			break;
#endif
#ifdef HAVE_SS7
		case SIG_SS7:
			ast_mutex_unlock(&ss7->lock);
			break;
#endif
		default:
			break;
		}
	}
#endif
	for (;;) {
		if (p->owner) {
			if (ast_channel_trylock(p->owner)) {
				DEADLOCK_AVOIDANCE(&p->lock);
			} else {
				ast_queue_frame(p->owner, f);
				ast_channel_unlock(p->owner);
				break;
			}
		} else
			break;
	}
#if defined(HAVE_PRI) || defined(HAVE_SS7)
	if (data) {
		switch (p->sig) {
#ifdef HAVE_PRI
		case SIG_BRI:
		case SIG_BRI_PTMP:
		case SIG_PRI:
			ast_mutex_lock(&pri->lock);
			break;
#endif
#ifdef HAVE_SS7
		case SIG_SS7:
			ast_mutex_lock(&ss7->lock);
			break;
#endif
		default:
			break;
		}
	}

#endif
}

static int restore_gains(struct dahdi_pvt *p);

static void swap_subs(struct dahdi_pvt *p, int a, int b)
{
	int tchan;
	int tinthreeway;
	struct ast_channel *towner;

	ast_debug(1, "Swapping %d and %d\n", a, b);

	tchan = p->subs[a].chan;
	towner = p->subs[a].owner;
	tinthreeway = p->subs[a].inthreeway;

	p->subs[a].chan = p->subs[b].chan;
	p->subs[a].owner = p->subs[b].owner;
	p->subs[a].inthreeway = p->subs[b].inthreeway;

	p->subs[b].chan = tchan;
	p->subs[b].owner = towner;
	p->subs[b].inthreeway = tinthreeway;

	if (p->subs[a].owner)
		ast_channel_set_fd(p->subs[a].owner, 0, p->subs[a].dfd);
	if (p->subs[b].owner)
		ast_channel_set_fd(p->subs[b].owner, 0, p->subs[b].dfd);
	wakeup_sub(p, a, NULL);
	wakeup_sub(p, b, NULL);
}

static int dahdi_open(char *fn)
{
	int fd;
	int isnum;
	int chan = 0;
	int bs;
	int x;
	isnum = 1;
	for (x = 0; x < strlen(fn); x++) {
		if (!isdigit(fn[x])) {
			isnum = 0;
			break;
		}
	}
	if (isnum) {
		chan = atoi(fn);
		if (chan < 1) {
			ast_log(LOG_WARNING, "Invalid channel number '%s'\n", fn);
			return -1;
		}
		fn = "/dev/dahdi/channel";
	}
	fd = open(fn, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		ast_log(LOG_WARNING, "Unable to open '%s': %s\n", fn, strerror(errno));
		return -1;
	}
	if (chan) {
		if (ioctl(fd, DAHDI_SPECIFY, &chan)) {
			x = errno;
			close(fd);
			errno = x;
			ast_log(LOG_WARNING, "Unable to specify channel %d: %s\n", chan, strerror(errno));
			return -1;
		}
	}
	bs = READ_SIZE;
	if (ioctl(fd, DAHDI_SET_BLOCKSIZE, &bs) == -1) {
		ast_log(LOG_WARNING, "Unable to set blocksize '%d': %s\n", bs,  strerror(errno));
		x = errno;
		close(fd);
		errno = x;
		return -1;
	}
	return fd;
}

static void dahdi_close(int fd)
{
	if (fd > 0)
		close(fd);
}

static int dahdi_setlinear(int dfd, int linear)
{
	int res;
	res = ioctl(dfd, DAHDI_SETLINEAR, &linear);
	if (res)
		return res;
	return 0;
}


static int alloc_sub(struct dahdi_pvt *p, int x)
{
	struct dahdi_bufferinfo bi;
	int res;
	if (p->subs[x].dfd >= 0) {
		ast_log(LOG_WARNING, "%s subchannel of %d already in use\n", subnames[x], p->channel);
		return -1;
	}

	p->subs[x].dfd = dahdi_open("/dev/dahdi/pseudo");
	if (p->subs[x].dfd <= -1) {
		ast_log(LOG_WARNING, "Unable to open pseudo channel: %s\n", strerror(errno));
		return -1;
	}

	res = ioctl(p->subs[x].dfd, DAHDI_GET_BUFINFO, &bi);
	if (!res) {
		bi.txbufpolicy = p->buf_policy;
		bi.rxbufpolicy = p->buf_policy;
		bi.numbufs = p->buf_no;
		res = ioctl(p->subs[x].dfd, DAHDI_SET_BUFINFO, &bi);
		if (res < 0) {
			ast_log(LOG_WARNING, "Unable to set buffer policy on channel %d: %s\n", x, strerror(errno));
		}
	} else
		ast_log(LOG_WARNING, "Unable to check buffer policy on channel %d: %s\n", x, strerror(errno));

	if (ioctl(p->subs[x].dfd, DAHDI_CHANNO, &p->subs[x].chan) == 1) {
		ast_log(LOG_WARNING, "Unable to get channel number for pseudo channel on FD %d: %s\n", p->subs[x].dfd, strerror(errno));
		dahdi_close(p->subs[x].dfd);
		p->subs[x].dfd = -1;
		return -1;
	}
	ast_debug(1, "Allocated %s subchannel on FD %d channel %d\n", subnames[x], p->subs[x].dfd, p->subs[x].chan);
	return 0;
}

static int unalloc_sub(struct dahdi_pvt *p, int x)
{
	if (!x) {
		ast_log(LOG_WARNING, "Trying to unalloc the real channel %d?!?\n", p->channel);
		return -1;
	}
	ast_debug(1, "Released sub %d of channel %d\n", x, p->channel);
	if (p->subs[x].dfd > -1) {
		dahdi_close(p->subs[x].dfd);
	}
	p->subs[x].dfd = -1;
	p->subs[x].linear = 0;
	p->subs[x].chan = 0;
	p->subs[x].owner = NULL;
	p->subs[x].inthreeway = 0;
	p->polarity = POLARITY_IDLE;
	memset(&p->subs[x].curconf, 0, sizeof(p->subs[x].curconf));
	return 0;
}

static int digit_to_dtmfindex(char digit)
{
	if (isdigit(digit))
		return DAHDI_TONE_DTMF_BASE + (digit - '0');
	else if (digit >= 'A' && digit <= 'D')
		return DAHDI_TONE_DTMF_A + (digit - 'A');
	else if (digit >= 'a' && digit <= 'd')
		return DAHDI_TONE_DTMF_A + (digit - 'a');
	else if (digit == '*')
		return DAHDI_TONE_DTMF_s;
	else if (digit == '#')
		return DAHDI_TONE_DTMF_p;
	else
		return -1;
}

static int dahdi_digit_begin(struct ast_channel *chan, char digit)
{
	struct dahdi_pvt *pvt;
	int index;
	int dtmf = -1;

	pvt = chan->tech_pvt;

	ast_mutex_lock(&pvt->lock);

	index = dahdi_get_index(chan, pvt, 0);

	if ((index != SUB_REAL) || !pvt->owner)
		goto out;

#ifdef HAVE_PRI
	if (((pvt->sig == SIG_PRI) || (pvt->sig == SIG_BRI) || (pvt->sig == SIG_BRI_PTMP))
			&& (chan->_state == AST_STATE_DIALING) && !pvt->proceeding) {
		if (pvt->setup_ack) {
			if (!pri_grab(pvt, pvt->pri)) {
				pri_information(pvt->pri->pri, pvt->call, digit);
				pri_rel(pvt->pri);
			} else
				ast_log(LOG_WARNING, "Unable to grab PRI on span %d\n", pvt->span);
		} else if (strlen(pvt->dialdest) < sizeof(pvt->dialdest) - 1) {
			int res;
			ast_debug(1, "Queueing digit '%c' since setup_ack not yet received\n", digit);
			res = strlen(pvt->dialdest);
			pvt->dialdest[res++] = digit;
			pvt->dialdest[res] = '\0';
		}
		goto out;
	}
#endif
	if ((dtmf = digit_to_dtmfindex(digit)) == -1)
		goto out;

	if (pvt->pulse || ioctl(pvt->subs[SUB_REAL].dfd, DAHDI_SENDTONE, &dtmf)) {
		int res;
		struct dahdi_dialoperation zo = {
			.op = DAHDI_DIAL_OP_APPEND,
		};

		zo.dialstr[0] = 'T';
		zo.dialstr[1] = digit;
		zo.dialstr[2] = '\0';
		if ((res = ioctl(pvt->subs[SUB_REAL].dfd, DAHDI_DIAL, &zo)))
			ast_log(LOG_WARNING, "Couldn't dial digit %c: %s\n", digit, strerror(errno));
		else
			pvt->dialing = 1;
	} else {
		ast_debug(1, "Started VLDTMF digit '%c'\n", digit);
		pvt->dialing = 1;
		pvt->begindigit = digit;
	}

out:
	ast_mutex_unlock(&pvt->lock);

	return 0;
}

static int dahdi_digit_end(struct ast_channel *chan, char digit, unsigned int duration)
{
	struct dahdi_pvt *pvt;
	int res = 0;
	int index;
	int x;

	pvt = chan->tech_pvt;

	ast_mutex_lock(&pvt->lock);

	index = dahdi_get_index(chan, pvt, 0);

	if ((index != SUB_REAL) || !pvt->owner || pvt->pulse)
		goto out;

#ifdef HAVE_PRI
	/* This means that the digit was already sent via PRI signalling */
	if (((pvt->sig == SIG_PRI) || (pvt->sig == SIG_BRI) || (pvt->sig == SIG_BRI_PTMP))
			&& !pvt->begindigit)
		goto out;
#endif

	if (pvt->begindigit) {
		x = -1;
		ast_debug(1, "Ending VLDTMF digit '%c'\n", digit);
		res = ioctl(pvt->subs[SUB_REAL].dfd, DAHDI_SENDTONE, &x);
		pvt->dialing = 0;
		pvt->begindigit = 0;
	}

out:
	ast_mutex_unlock(&pvt->lock);

	return res;
}

static char *events[] = {
	"No event",
	"On hook",
	"Ring/Answered",
	"Wink/Flash",
	"Alarm",
	"No more alarm",
	"HDLC Abort",
	"HDLC Overrun",
	"HDLC Bad FCS",
	"Dial Complete",
	"Ringer On",
	"Ringer Off",
	"Hook Transition Complete",
	"Bits Changed",
	"Pulse Start",
	"Timer Expired",
	"Timer Ping",
	"Polarity Reversal",
	"Ring Begin",
};

static struct {
	int alarm;
	char *name;
} alarms[] = {
	{ DAHDI_ALARM_RED, "Red Alarm" },
	{ DAHDI_ALARM_YELLOW, "Yellow Alarm" },
	{ DAHDI_ALARM_BLUE, "Blue Alarm" },
	{ DAHDI_ALARM_RECOVER, "Recovering" },
	{ DAHDI_ALARM_LOOPBACK, "Loopback" },
	{ DAHDI_ALARM_NOTOPEN, "Not Open" },
	{ DAHDI_ALARM_NONE, "None" },
};

static char *alarm2str(int alarm)
{
	int x;
	for (x = 0; x < ARRAY_LEN(alarms); x++) {
		if (alarms[x].alarm & alarm)
			return alarms[x].name;
	}
	return alarm ? "Unknown Alarm" : "No Alarm";
}

static char *event2str(int event)
{
	static char buf[256];
	if ((event < (ARRAY_LEN(events))) && (event > -1))
		return events[event];
	sprintf(buf, "Event %d", event); /* safe */
	return buf;
}

#ifdef HAVE_PRI
static char *dialplan2str(int dialplan)
{
	if (dialplan == -1 || dialplan == -2) {
		return("Dynamically set dialplan in ISDN");
	}
	return (pri_plan2str(dialplan));
}
#endif

static char *dahdi_sig2str(int sig)
{
	static char buf[256];
	switch (sig) {
	case SIG_EM:
		return "E & M Immediate";
	case SIG_EMWINK:
		return "E & M Wink";
	case SIG_EM_E1:
		return "E & M E1";
	case SIG_FEATD:
		return "Feature Group D (DTMF)";
	case SIG_FEATDMF:
		return "Feature Group D (MF)";
	case SIG_FEATDMF_TA:
		return "Feature Groud D (MF) Tandem Access";
	case SIG_FEATB:
		return "Feature Group B (MF)";
	case SIG_E911:
		return "E911 (MF)";
	case SIG_FGC_CAMA:
		return "FGC/CAMA (Dialpulse)";
	case SIG_FGC_CAMAMF:
		return "FGC/CAMA (MF)";
	case SIG_FXSLS:
		return "FXS Loopstart";
	case SIG_FXSGS:
		return "FXS Groundstart";
	case SIG_FXSKS:
		return "FXS Kewlstart";
	case SIG_FXOLS:
		return "FXO Loopstart";
	case SIG_FXOGS:
		return "FXO Groundstart";
	case SIG_FXOKS:
		return "FXO Kewlstart";
	case SIG_PRI:
		return "ISDN PRI";
	case SIG_BRI:
		return "ISDN BRI Point to Point";
	case SIG_BRI_PTMP:
		return "ISDN BRI Point to MultiPoint";
	case SIG_SS7:
		return "SS7";
	case SIG_SF:
		return "SF (Tone) Immediate";
	case SIG_SFWINK:
		return "SF (Tone) Wink";
	case SIG_SF_FEATD:
		return "SF (Tone) with Feature Group D (DTMF)";
	case SIG_SF_FEATDMF:
		return "SF (Tone) with Feature Group D (MF)";
	case SIG_SF_FEATB:
		return "SF (Tone) with Feature Group B (MF)";
	case SIG_GR303FXOKS:
		return "GR-303 with FXOKS";
	case SIG_GR303FXSKS:
		return "GR-303 with FXSKS";
	case 0:
		return "Pseudo";
	default:
		snprintf(buf, sizeof(buf), "Unknown signalling %d", sig);
		return buf;
	}
}

#define sig2str dahdi_sig2str

static int conf_add(struct dahdi_pvt *p, struct dahdi_subchannel *c, int index, int slavechannel)
{
	/* If the conference already exists, and we're already in it
	   don't bother doing anything */
	struct dahdi_confinfo zi;

	memset(&zi, 0, sizeof(zi));
	zi.chan = 0;

	if (slavechannel > 0) {
		/* If we have only one slave, do a digital mon */
		zi.confmode = DAHDI_CONF_DIGITALMON;
		zi.confno = slavechannel;
	} else {
		if (!index) {
			/* Real-side and pseudo-side both participate in conference */
			zi.confmode = DAHDI_CONF_REALANDPSEUDO | DAHDI_CONF_TALKER | DAHDI_CONF_LISTENER |
				DAHDI_CONF_PSEUDO_TALKER | DAHDI_CONF_PSEUDO_LISTENER;
		} else
			zi.confmode = DAHDI_CONF_CONF | DAHDI_CONF_TALKER | DAHDI_CONF_LISTENER;
		zi.confno = p->confno;
	}
	if ((zi.confno == c->curconf.confno) && (zi.confmode == c->curconf.confmode))
		return 0;
	if (c->dfd < 0)
		return 0;
	if (ioctl(c->dfd, DAHDI_SETCONF, &zi)) {
		ast_log(LOG_WARNING, "Failed to add %d to conference %d/%d: %s\n", c->dfd, zi.confmode, zi.confno, strerror(errno));
		return -1;
	}
	if (slavechannel < 1) {
		p->confno = zi.confno;
	}
	memcpy(&c->curconf, &zi, sizeof(c->curconf));
	ast_debug(1, "Added %d to conference %d/%d\n", c->dfd, c->curconf.confmode, c->curconf.confno);
	return 0;
}

static int isourconf(struct dahdi_pvt *p, struct dahdi_subchannel *c)
{
	/* If they're listening to our channel, they're ours */
	if ((p->channel == c->curconf.confno) && (c->curconf.confmode == DAHDI_CONF_DIGITALMON))
		return 1;
	/* If they're a talker on our (allocated) conference, they're ours */
	if ((p->confno > 0) && (p->confno == c->curconf.confno) && (c->curconf.confmode & DAHDI_CONF_TALKER))
		return 1;
	return 0;
}

static int conf_del(struct dahdi_pvt *p, struct dahdi_subchannel *c, int index)
{
	struct dahdi_confinfo zi;
	if (/* Can't delete if there's no dfd */
		(c->dfd < 0) ||
		/* Don't delete from the conference if it's not our conference */
		!isourconf(p, c)
		/* Don't delete if we don't think it's conferenced at all (implied) */
		) return 0;
	memset(&zi, 0, sizeof(zi));
	if (ioctl(c->dfd, DAHDI_SETCONF, &zi)) {
		ast_log(LOG_WARNING, "Failed to drop %d from conference %d/%d: %s\n", c->dfd, c->curconf.confmode, c->curconf.confno, strerror(errno));
		return -1;
	}
	ast_debug(1, "Removed %d from conference %d/%d\n", c->dfd, c->curconf.confmode, c->curconf.confno);
	memcpy(&c->curconf, &zi, sizeof(c->curconf));
	return 0;
}

static int isslavenative(struct dahdi_pvt *p, struct dahdi_pvt **out)
{
	int x;
	int useslavenative;
	struct dahdi_pvt *slave = NULL;
	/* Start out optimistic */
	useslavenative = 1;
	/* Update conference state in a stateless fashion */
	for (x = 0; x < 3; x++) {
		/* Any three-way calling makes slave native mode *definitely* out
		   of the question */
		if ((p->subs[x].dfd > -1) && p->subs[x].inthreeway)
			useslavenative = 0;
	}
	/* If we don't have any 3-way calls, check to see if we have
	   precisely one slave */
	if (useslavenative) {
		for (x = 0; x < MAX_SLAVES; x++) {
			if (p->slaves[x]) {
				if (slave) {
					/* Whoops already have a slave!  No
					   slave native and stop right away */
					slave = NULL;
					useslavenative = 0;
					break;
				} else {
					/* We have one slave so far */
					slave = p->slaves[x];
				}
			}
		}
	}
	/* If no slave, slave native definitely out */
	if (!slave)
		useslavenative = 0;
	else if (slave->law != p->law) {
		useslavenative = 0;
		slave = NULL;
	}
	if (out)
		*out = slave;
	return useslavenative;
}

static int reset_conf(struct dahdi_pvt *p)
{
	p->confno = -1;
	memset(&p->subs[SUB_REAL].curconf, 0, sizeof(p->subs[SUB_REAL].curconf));
	if (p->subs[SUB_REAL].dfd > -1) {
		struct dahdi_confinfo zi;
		memset(&zi, 0, sizeof(zi));
		if (ioctl(p->subs[SUB_REAL].dfd, DAHDI_SETCONF, &zi))
			ast_log(LOG_WARNING, "Failed to reset conferencing on channel %d: %s\n", p->channel, strerror(errno));
	}
	return 0;
}

static int update_conf(struct dahdi_pvt *p)
{
	int needconf = 0;
	int x;
	int useslavenative;
	struct dahdi_pvt *slave = NULL;

	useslavenative = isslavenative(p, &slave);
	/* Start with the obvious, general stuff */
	for (x = 0; x < 3; x++) {
		/* Look for three way calls */
		if ((p->subs[x].dfd > -1) && p->subs[x].inthreeway) {
			conf_add(p, &p->subs[x], x, 0);
			needconf++;
		} else {
			conf_del(p, &p->subs[x], x);
		}
	}
	/* If we have a slave, add him to our conference now. or DAX
	   if this is slave native */
	for (x = 0; x < MAX_SLAVES; x++) {
		if (p->slaves[x]) {
			if (useslavenative)
				conf_add(p, &p->slaves[x]->subs[SUB_REAL], SUB_REAL, GET_CHANNEL(p));
			else {
				conf_add(p, &p->slaves[x]->subs[SUB_REAL], SUB_REAL, 0);
				needconf++;
			}
		}
	}
	/* If we're supposed to be in there, do so now */
	if (p->inconference && !p->subs[SUB_REAL].inthreeway) {
		if (useslavenative)
			conf_add(p, &p->subs[SUB_REAL], SUB_REAL, GET_CHANNEL(slave));
		else {
			conf_add(p, &p->subs[SUB_REAL], SUB_REAL, 0);
			needconf++;
		}
	}
	/* If we have a master, add ourselves to his conference */
	if (p->master) {
		if (isslavenative(p->master, NULL)) {
			conf_add(p->master, &p->subs[SUB_REAL], SUB_REAL, GET_CHANNEL(p->master));
		} else {
			conf_add(p->master, &p->subs[SUB_REAL], SUB_REAL, 0);
		}
	}
	if (!needconf) {
		/* Nobody is left (or should be left) in our conference.
		   Kill it. */
		p->confno = -1;
	}
	ast_debug(1, "Updated conferencing on %d, with %d conference users\n", p->channel, needconf);
	return 0;
}

static void dahdi_enable_ec(struct dahdi_pvt *p)
{
	int x;
	int res;
	if (!p)
		return;
	if (p->echocanon) {
		ast_debug(1, "Echo cancellation already on\n");
		return;
	}
	if (p->digital) {
		ast_debug(1, "Echo cancellation isn't required on digital connection\n");
		return;
	}
	if (p->echocancel.head.tap_length) {
		if ((p->sig == SIG_BRI) || (p->sig == SIG_BRI_PTMP) || (p->sig == SIG_PRI) || (p->sig == SIG_SS7)) {
			x = 1;
			res = ioctl(p->subs[SUB_REAL].dfd, DAHDI_AUDIOMODE, &x);
			if (res)
				ast_log(LOG_WARNING, "Unable to enable audio mode on channel %d (%s)\n", p->channel, strerror(errno));
		}
		res = ioctl(p->subs[SUB_REAL].dfd, DAHDI_ECHOCANCEL_PARAMS, &p->echocancel);
		if (res)  {
			ast_log(LOG_WARNING, "Unable to enable echo cancellation on channel %d (%s)\n", p->channel, strerror(errno));
		} else {
			p->echocanon = 1;
			ast_debug(1, "Enabled echo cancellation on channel %d\n", p->channel);
		}
	} else
		ast_debug(1, "No echo cancellation requested\n");
}

static void dahdi_train_ec(struct dahdi_pvt *p)
{
	int x;
	int res;

	if (p && p->echocanon && p->echotraining) {
		x = p->echotraining;
		res = ioctl(p->subs[SUB_REAL].dfd, DAHDI_ECHOTRAIN, &x);
		if (res)
			ast_log(LOG_WARNING, "Unable to request echo training on channel %d: %s\n", p->channel, strerror(errno));
		else
			ast_debug(1, "Engaged echo training on channel %d\n", p->channel);
	} else {
		ast_debug(1, "No echo training requested\n");
	}
}

static void dahdi_disable_ec(struct dahdi_pvt *p)
{
	int res;

	if (p->echocanon) {
		struct dahdi_echocanparams ecp = { .tap_length = 0 };

		res = ioctl(p->subs[SUB_REAL].dfd, DAHDI_ECHOCANCEL_PARAMS, &ecp);

		if (res)
			ast_log(LOG_WARNING, "Unable to disable echo cancellation on channel %d: %s\n", p->channel, strerror(errno));
		else
			ast_debug(1, "Disabled echo cancellation on channel %d\n", p->channel);
	}

	p->echocanon = 0;
}

static void fill_txgain(struct dahdi_gains *g, float gain, int law)
{
	int j;
	int k;
	float linear_gain = pow(10.0, gain / 20.0);

	switch (law) {
	case DAHDI_LAW_ALAW:
		for (j = 0; j < ARRAY_LEN(g->txgain); j++) {
			if (gain) {
				k = (int) (((float) AST_ALAW(j)) * linear_gain);
				if (k > 32767) k = 32767;
				if (k < -32767) k = -32767;
				g->txgain[j] = AST_LIN2A(k);
			} else {
				g->txgain[j] = j;
			}
		}
		break;
	case DAHDI_LAW_MULAW:
		for (j = 0; j < ARRAY_LEN(g->txgain); j++) {
			if (gain) {
				k = (int) (((float) AST_MULAW(j)) * linear_gain);
				if (k > 32767) k = 32767;
				if (k < -32767) k = -32767;
				g->txgain[j] = AST_LIN2MU(k);
			} else {
				g->txgain[j] = j;
			}
		}
		break;
	}
}

static void fill_rxgain(struct dahdi_gains *g, float gain, int law)
{
	int j;
	int k;
	float linear_gain = pow(10.0, gain / 20.0);

	switch (law) {
	case DAHDI_LAW_ALAW:
		for (j = 0; j < ARRAY_LEN(g->rxgain); j++) {
			if (gain) {
				k = (int) (((float) AST_ALAW(j)) * linear_gain);
				if (k > 32767) k = 32767;
				if (k < -32767) k = -32767;
				g->rxgain[j] = AST_LIN2A(k);
			} else {
				g->rxgain[j] = j;
			}
		}
		break;
	case DAHDI_LAW_MULAW:
		for (j = 0; j < ARRAY_LEN(g->rxgain); j++) {
			if (gain) {
				k = (int) (((float) AST_MULAW(j)) * linear_gain);
				if (k > 32767) k = 32767;
				if (k < -32767) k = -32767;
				g->rxgain[j] = AST_LIN2MU(k);
			} else {
				g->rxgain[j] = j;
			}
		}
		break;
	}
}

static int set_actual_txgain(int fd, int chan, float gain, int law)
{
	struct dahdi_gains g;
	int res;

	memset(&g, 0, sizeof(g));
	g.chan = chan;
	res = ioctl(fd, DAHDI_GETGAINS, &g);
	if (res) {
		ast_debug(1, "Failed to read gains: %s\n", strerror(errno));
		return res;
	}

	fill_txgain(&g, gain, law);

	return ioctl(fd, DAHDI_SETGAINS, &g);
}

static int set_actual_rxgain(int fd, int chan, float gain, int law)
{
	struct dahdi_gains g;
	int res;

	memset(&g, 0, sizeof(g));
	g.chan = chan;
	res = ioctl(fd, DAHDI_GETGAINS, &g);
	if (res) {
		ast_debug(1, "Failed to read gains: %s\n", strerror(errno));
		return res;
	}

	fill_rxgain(&g, gain, law);

	return ioctl(fd, DAHDI_SETGAINS, &g);
}

static int set_actual_gain(int fd, int chan, float rxgain, float txgain, int law)
{
	return set_actual_txgain(fd, chan, txgain, law) | set_actual_rxgain(fd, chan, rxgain, law);
}

static int bump_gains(struct dahdi_pvt *p)
{
	int res;

	/* Bump receive gain by value stored in cid_rxgain */
	res = set_actual_gain(p->subs[SUB_REAL].dfd, 0, p->rxgain + p->cid_rxgain, p->txgain, p->law);
	if (res) {
		ast_log(LOG_WARNING, "Unable to bump gain: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int restore_gains(struct dahdi_pvt *p)
{
	int res;

	res = set_actual_gain(p->subs[SUB_REAL].dfd, 0, p->rxgain, p->txgain, p->law);
	if (res) {
		ast_log(LOG_WARNING, "Unable to restore gains: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static inline int dahdi_set_hook(int fd, int hs)
{
	int x, res;

	x = hs;
	res = ioctl(fd, DAHDI_HOOK, &x);

	if (res < 0) {
		if (errno == EINPROGRESS)
			return 0;
		ast_log(LOG_WARNING, "DAHDI hook failed returned %d (trying %d): %s\n", res, hs, strerror(errno));
		/* will expectedly fail if phone is off hook during operation, such as during a restart */
	}

	return res;
}

static inline int dahdi_confmute(struct dahdi_pvt *p, int muted)
{
	int x, y, res;
	x = muted;
	if ((p->sig == SIG_PRI) || (p->sig == SIG_SS7) || (p->sig == SIG_BRI) || (p->sig == SIG_BRI_PTMP)) {
		y = 1;
		res = ioctl(p->subs[SUB_REAL].dfd, DAHDI_AUDIOMODE, &y);
		if (res)
			ast_log(LOG_WARNING, "Unable to set audio mode on %d: %s\n", p->channel, strerror(errno));
	}
	res = ioctl(p->subs[SUB_REAL].dfd, DAHDI_CONFMUTE, &x);
	if (res < 0)
		ast_log(LOG_WARNING, "DAHDI confmute(%d) failed on channel %d: %s\n", muted, p->channel, strerror(errno));
	return res;
}

static int save_conference(struct dahdi_pvt *p)
{
	struct dahdi_confinfo c;
	int res;
	if (p->saveconf.confmode) {
		ast_log(LOG_WARNING, "Can't save conference -- already in use\n");
		return -1;
	}
	p->saveconf.chan = 0;
	res = ioctl(p->subs[SUB_REAL].dfd, DAHDI_GETCONF, &p->saveconf);
	if (res) {
		ast_log(LOG_WARNING, "Unable to get conference info: %s\n", strerror(errno));
		p->saveconf.confmode = 0;
		return -1;
	}
	memset(&c, 0, sizeof(c));
	c.confmode = DAHDI_CONF_NORMAL;
	res = ioctl(p->subs[SUB_REAL].dfd, DAHDI_SETCONF, &c);
	if (res) {
		ast_log(LOG_WARNING, "Unable to set conference info: %s\n", strerror(errno));
		return -1;
	}
	ast_debug(1, "Disabled conferencing\n");
	return 0;
}

/*!
 * \brief Send MWI state change
 *
 * \arg mailbox_full This is the mailbox associated with the FXO line that the
 *      MWI state has changed on.
 * \arg thereornot This argument should simply be set to 1 or 0, to indicate
 *      whether there are messages waiting or not.
 *
 *  \return nothing
 *
 * This function does two things:
 *
 * 1) It generates an internal Asterisk event notifying any other module that
 *    cares about MWI that the state of a mailbox has changed.
 *
 * 2) It runs the script specified by the mwimonitornotify option to allow
 *    some custom handling of the state change.
 */
static void notify_message(char *mailbox_full, int thereornot)
{
	char s[sizeof(mwimonitornotify) + 80];
	struct ast_event *event;
	char *mailbox, *context;

	/* Strip off @default */
	context = mailbox = ast_strdupa(mailbox_full);
	strsep(&context, "@");
	if (ast_strlen_zero(context))
		context = "default";

	if (!(event = ast_event_new(AST_EVENT_MWI,
			AST_EVENT_IE_MAILBOX, AST_EVENT_IE_PLTYPE_STR, mailbox,
			AST_EVENT_IE_CONTEXT, AST_EVENT_IE_PLTYPE_STR, context,
			AST_EVENT_IE_NEWMSGS, AST_EVENT_IE_PLTYPE_UINT, thereornot,
			AST_EVENT_IE_OLDMSGS, AST_EVENT_IE_PLTYPE_UINT, thereornot,
			AST_EVENT_IE_END))) {
		return;
	}

	ast_event_queue_and_cache(event,
		AST_EVENT_IE_MAILBOX, AST_EVENT_IE_PLTYPE_STR,
		AST_EVENT_IE_CONTEXT, AST_EVENT_IE_PLTYPE_STR,
		AST_EVENT_IE_END);

	if (!ast_strlen_zero(mailbox) && !ast_strlen_zero(mwimonitornotify)) {
		snprintf(s, sizeof(s), "%s %s %d", mwimonitornotify, mailbox, thereornot);
		ast_safe_system(s);
	}
}

static int restore_conference(struct dahdi_pvt *p)
{
	int res;
	if (p->saveconf.confmode) {
		res = ioctl(p->subs[SUB_REAL].dfd, DAHDI_SETCONF, &p->saveconf);
		p->saveconf.confmode = 0;
		if (res) {
			ast_log(LOG_WARNING, "Unable to restore conference info: %s\n", strerror(errno));
			return -1;
		}
	}
	ast_debug(1, "Restored conferencing\n");
	return 0;
}

static int send_callerid(struct dahdi_pvt *p);

static int send_cwcidspill(struct dahdi_pvt *p)
{
	p->callwaitcas = 0;
	p->cidcwexpire = 0;
	if (!(p->cidspill = ast_malloc(MAX_CALLERID_SIZE)))
		return -1;
	p->cidlen = ast_callerid_callwaiting_generate(p->cidspill, p->callwait_name, p->callwait_num, AST_LAW(p));
	/* Make sure we account for the end */
	p->cidlen += READ_SIZE * 4;
	p->cidpos = 0;
	send_callerid(p);
	ast_verb(3, "CPE supports Call Waiting Caller*ID.  Sending '%s/%s'\n", p->callwait_name, p->callwait_num);
	return 0;
}

static int has_voicemail(struct dahdi_pvt *p)
{
	int new_msgs;
	struct ast_event *event;
	char *mailbox, *context;

	mailbox = context = ast_strdupa(p->mailbox);
	strsep(&context, "@");
	if (ast_strlen_zero(context))
		context = "default";

	event = ast_event_get_cached(AST_EVENT_MWI,
		AST_EVENT_IE_MAILBOX, AST_EVENT_IE_PLTYPE_STR, mailbox,
		AST_EVENT_IE_CONTEXT, AST_EVENT_IE_PLTYPE_STR, context,
		AST_EVENT_IE_NEWMSGS, AST_EVENT_IE_PLTYPE_EXISTS,
		AST_EVENT_IE_END);

	if (event) {
		new_msgs = ast_event_get_ie_uint(event, AST_EVENT_IE_NEWMSGS);
		ast_event_destroy(event);
	} else
		new_msgs = ast_app_has_voicemail(p->mailbox, NULL);

	return new_msgs;
}

static int send_callerid(struct dahdi_pvt *p)
{
	/* Assumes spill in p->cidspill, p->cidlen in length and we're p->cidpos into it */
	int res;
	/* Take out of linear mode if necessary */
	if (p->subs[SUB_REAL].linear) {
		p->subs[SUB_REAL].linear = 0;
		dahdi_setlinear(p->subs[SUB_REAL].dfd, 0);
	}
	while (p->cidpos < p->cidlen) {
		res = write(p->subs[SUB_REAL].dfd, p->cidspill + p->cidpos, p->cidlen - p->cidpos);
		if (res < 0) {
			if (errno == EAGAIN)
				return 0;
			else {
				ast_log(LOG_WARNING, "write failed: %s\n", strerror(errno));
				return -1;
			}
		}
		if (!res)
			return 0;
		p->cidpos += res;
	}
	ast_free(p->cidspill);
	p->cidspill = NULL;
	if (p->callwaitcas) {
		/* Wait for CID/CW to expire */
		p->cidcwexpire = CIDCW_EXPIRE_SAMPLES;
	} else
		restore_conference(p);
	return 0;
}

static int dahdi_callwait(struct ast_channel *ast)
{
	struct dahdi_pvt *p = ast->tech_pvt;
	p->callwaitingrepeat = CALLWAITING_REPEAT_SAMPLES;
	if (p->cidspill) {
		ast_log(LOG_WARNING, "Spill already exists?!?\n");
		ast_free(p->cidspill);
	}
	if (!(p->cidspill = ast_malloc(2400 /* SAS */ + 680 /* CAS */ + READ_SIZE * 4)))
		return -1;
	save_conference(p);
	/* Silence */
	memset(p->cidspill, 0x7f, 2400 + 600 + READ_SIZE * 4);
	if (!p->callwaitrings && p->callwaitingcallerid) {
		ast_gen_cas(p->cidspill, 1, 2400 + 680, AST_LAW(p));
		p->callwaitcas = 1;
		p->cidlen = 2400 + 680 + READ_SIZE * 4;
	} else {
		ast_gen_cas(p->cidspill, 1, 2400, AST_LAW(p));
		p->callwaitcas = 0;
		p->cidlen = 2400 + READ_SIZE * 4;
	}
	p->cidpos = 0;
	send_callerid(p);

	return 0;
}

#ifdef HAVE_SS7
static unsigned char cid_pres2ss7pres(int cid_pres)
{
	 return (cid_pres >> 5) & 0x03;
}

static unsigned char cid_pres2ss7screen(int cid_pres)
{
	return cid_pres & 0x03;
}

static int ss7_parse_prefix(struct dahdi_pvt *p, const char *number, char *nai)
{
	int strip = 0;

	if (strncmp(number, p->ss7->internationalprefix, strlen(p->ss7->internationalprefix)) == 0) {
		strip = strlen(p->ss7->internationalprefix);
		*nai = SS7_NAI_INTERNATIONAL;
	} else if (strncmp(number, p->ss7->nationalprefix, strlen(p->ss7->nationalprefix)) == 0) {
		strip = strlen(p->ss7->nationalprefix);
		*nai = SS7_NAI_NATIONAL;
	} else if (strncmp(number, p->ss7->networkroutedprefix, strlen(p->ss7->networkroutedprefix)) == 0) {
		strip = strlen(p->ss7->networkroutedprefix);
		*nai = SS7_NAI_NETWORKROUTED;
	} else if (strncmp(number, p->ss7->unknownprefix, strlen(p->ss7->unknownprefix)) == 0) {
		strip = strlen(p->ss7->unknownprefix);
		*nai = SS7_NAI_UNKNOWN;
	} else if (strncmp(number, p->ss7->subscriberprefix, strlen(p->ss7->subscriberprefix)) == 0) {
		strip = strlen(p->ss7->subscriberprefix);
		*nai = SS7_NAI_SUBSCRIBER;
	} else
		*nai = SS7_NAI_SUBSCRIBER;

	return strip;
}
#endif

static int dahdi_call(struct ast_channel *ast, char *rdest, int timeout)
{
	struct dahdi_pvt *p = ast->tech_pvt;
	int x, res, index,mysig;
	char *c, *n, *l;
#ifdef HAVE_PRI
	char *s = NULL;
#endif
	char dest[256]; /* must be same length as p->dialdest */
	ast_mutex_lock(&p->lock);
	ast_copy_string(dest, rdest, sizeof(dest));
	ast_copy_string(p->dialdest, rdest, sizeof(p->dialdest));
	if ((ast->_state == AST_STATE_BUSY)) {
		p->subs[SUB_REAL].needbusy = 1;
		ast_mutex_unlock(&p->lock);
		return 0;
	}
	if ((ast->_state != AST_STATE_DOWN) && (ast->_state != AST_STATE_RESERVED)) {
		ast_log(LOG_WARNING, "dahdi_call called on %s, neither down nor reserved\n", ast->name);
		ast_mutex_unlock(&p->lock);
		return -1;
	}
	p->dialednone = 0;
	if ((p->radio || (p->oprmode < 0)))  /* if a radio channel, up immediately */
	{
		/* Special pseudo -- automatically up */
		ast_setstate(ast, AST_STATE_UP);
		ast_mutex_unlock(&p->lock);
		return 0;
	}
	x = DAHDI_FLUSH_READ | DAHDI_FLUSH_WRITE;
	res = ioctl(p->subs[SUB_REAL].dfd, DAHDI_FLUSH, &x);
	if (res)
		ast_log(LOG_WARNING, "Unable to flush input on channel %d: %s\n", p->channel, strerror(errno));
	p->outgoing = 1;

	set_actual_gain(p->subs[SUB_REAL].dfd, 0, p->rxgain, p->txgain, p->law);

	mysig = p->sig;
	if (p->outsigmod > -1)
		mysig = p->outsigmod;

	switch (mysig) {
	case SIG_FXOLS:
	case SIG_FXOGS:
	case SIG_FXOKS:
		if (p->owner == ast) {
			/* Normal ring, on hook */

			/* Don't send audio while on hook, until the call is answered */
			p->dialing = 1;
			if (p->use_callerid) {
				/* Generate the Caller-ID spill if desired */
				if (p->cidspill) {
					ast_log(LOG_WARNING, "cidspill already exists??\n");
					ast_free(p->cidspill);
				}
				p->callwaitcas = 0;
				if ((p->cidspill = ast_malloc(MAX_CALLERID_SIZE))) {
					p->cidlen = ast_callerid_generate(p->cidspill, ast->cid.cid_name, ast->cid.cid_num, AST_LAW(p));
					p->cidpos = 0;
					send_callerid(p);
				}
			}
			/* Choose proper cadence */
			if ((p->distinctivering > 0) && (p->distinctivering <= num_cadence)) {
				if (ioctl(p->subs[SUB_REAL].dfd, DAHDI_SETCADENCE, &cadences[p->distinctivering - 1]))
					ast_log(LOG_WARNING, "Unable to set distinctive ring cadence %d on '%s': %s\n", p->distinctivering, ast->name, strerror(errno));
				p->cidrings = cidrings[p->distinctivering - 1];
			} else {
				if (ioctl(p->subs[SUB_REAL].dfd, DAHDI_SETCADENCE, NULL))
					ast_log(LOG_WARNING, "Unable to reset default ring on '%s': %s\n", ast->name, strerror(errno));
				p->cidrings = p->sendcalleridafter;
			}

			/* nick@dccinc.com 4/3/03 mods to allow for deferred dialing */
			c = strchr(dest, '/');
			if (c)
				c++;
			if (c && (strlen(c) < p->stripmsd)) {
				ast_log(LOG_WARNING, "Number '%s' is shorter than stripmsd (%d)\n", c, p->stripmsd);
				c = NULL;
			}
			if (c) {
				p->dop.op = DAHDI_DIAL_OP_REPLACE;
				snprintf(p->dop.dialstr, sizeof(p->dop.dialstr), "Tw%s", c);
				ast_debug(1, "FXO: setup deferred dialstring: %s\n", c);
			} else {
				p->dop.dialstr[0] = '\0';
			}
			x = DAHDI_RING;
			if (ioctl(p->subs[SUB_REAL].dfd, DAHDI_HOOK, &x) && (errno != EINPROGRESS)) {
				ast_log(LOG_WARNING, "Unable to ring phone: %s\n", strerror(errno));
				ast_mutex_unlock(&p->lock);
				return -1;
			}
			p->dialing = 1;
		} else {
			/* Call waiting call */
			p->callwaitrings = 0;
			if (ast->cid.cid_num)
				ast_copy_string(p->callwait_num, ast->cid.cid_num, sizeof(p->callwait_num));
			else
				p->callwait_num[0] = '\0';
			if (ast->cid.cid_name)
				ast_copy_string(p->callwait_name, ast->cid.cid_name, sizeof(p->callwait_name));
			else
				p->callwait_name[0] = '\0';
			/* Call waiting tone instead */
			if (dahdi_callwait(ast)) {
				ast_mutex_unlock(&p->lock);
				return -1;
			}
			/* Make ring-back */
			if (tone_zone_play_tone(p->subs[SUB_CALLWAIT].dfd, DAHDI_TONE_RINGTONE))
				ast_log(LOG_WARNING, "Unable to generate call-wait ring-back on channel %s\n", ast->name);

		}
		n = ast->cid.cid_name;
		l = ast->cid.cid_num;
		if (l)
			ast_copy_string(p->lastcid_num, l, sizeof(p->lastcid_num));
		else
			p->lastcid_num[0] = '\0';
		if (n)
			ast_copy_string(p->lastcid_name, n, sizeof(p->lastcid_name));
		else
			p->lastcid_name[0] = '\0';
		ast_setstate(ast, AST_STATE_RINGING);
		index = dahdi_get_index(ast, p, 0);
		if (index > -1) {
			p->subs[index].needringing = 1;
		}
		break;
	case SIG_FXSLS:
	case SIG_FXSGS:
	case SIG_FXSKS:
	case SIG_EMWINK:
	case SIG_EM:
	case SIG_EM_E1:
	case SIG_FEATD:
	case SIG_FEATDMF:
	case SIG_E911:
	case SIG_FGC_CAMA:
	case SIG_FGC_CAMAMF:
	case SIG_FEATB:
	case SIG_SFWINK:
	case SIG_SF:
	case SIG_SF_FEATD:
	case SIG_SF_FEATDMF:
	case SIG_FEATDMF_TA:
	case SIG_SF_FEATB:
		c = strchr(dest, '/');
		if (c)
			c++;
		else
			c = "";
		if (strlen(c) < p->stripmsd) {
			ast_log(LOG_WARNING, "Number '%s' is shorter than stripmsd (%d)\n", c, p->stripmsd);
			ast_mutex_unlock(&p->lock);
			return -1;
		}
#ifdef HAVE_PRI
		/* Start the trunk, if not GR-303 */
		if (!p->pri) {
#endif
			x = DAHDI_START;
			res = ioctl(p->subs[SUB_REAL].dfd, DAHDI_HOOK, &x);
			if (res < 0) {
				if (errno != EINPROGRESS) {
					ast_log(LOG_WARNING, "Unable to start channel: %s\n", strerror(errno));
					ast_mutex_unlock(&p->lock);
					return -1;
				}
			}
#ifdef HAVE_PRI
		}
#endif
		ast_debug(1, "Dialing '%s'\n", c);
		p->dop.op = DAHDI_DIAL_OP_REPLACE;

		c += p->stripmsd;

		switch (mysig) {
		case SIG_FEATD:
			l = ast->cid.cid_num;
			if (l)
				snprintf(p->dop.dialstr, sizeof(p->dop.dialstr), "T*%s*%s*", l, c);
			else
				snprintf(p->dop.dialstr, sizeof(p->dop.dialstr), "T**%s*", c);
			break;
		case SIG_FEATDMF:
			l = ast->cid.cid_num;
			if (l)
				snprintf(p->dop.dialstr, sizeof(p->dop.dialstr), "M*00%s#*%s#", l, c);
			else
				snprintf(p->dop.dialstr, sizeof(p->dop.dialstr), "M*02#*%s#", c);
			break;
		case SIG_FEATDMF_TA:
		{
			const char *cic, *ozz;

			/* If you have to go through a Tandem Access point you need to use this */
			ozz = pbx_builtin_getvar_helper(p->owner, "FEATDMF_OZZ");
			if (!ozz)
				ozz = defaultozz;
			cic = pbx_builtin_getvar_helper(p->owner, "FEATDMF_CIC");
			if (!cic)
				cic = defaultcic;
			if (!ozz || !cic) {
				ast_log(LOG_WARNING, "Unable to dial channel of type feature group D MF tandem access without CIC or OZZ set\n");
				ast_mutex_unlock(&p->lock);
				return -1;
			}
			snprintf(p->dop.dialstr, sizeof(p->dop.dialstr), "M*%s%s#", ozz, cic);
			snprintf(p->finaldial, sizeof(p->finaldial), "M*%s#", c);
			p->whichwink = 0;
		}
			break;
		case SIG_E911:
			ast_copy_string(p->dop.dialstr, "M*911#", sizeof(p->dop.dialstr));
			break;
		case SIG_FGC_CAMA:
			snprintf(p->dop.dialstr, sizeof(p->dop.dialstr), "P%s", c);
			break;
		case SIG_FGC_CAMAMF:
		case SIG_FEATB:
			snprintf(p->dop.dialstr, sizeof(p->dop.dialstr), "M*%s#", c);
			break;
		default:
			if (p->pulse)
				snprintf(p->dop.dialstr, sizeof(p->dop.dialstr), "P%sw", c);
			else
				snprintf(p->dop.dialstr, sizeof(p->dop.dialstr), "T%sw", c);
			break;
		}

		if (p->echotraining && (strlen(p->dop.dialstr) > 4)) {
			memset(p->echorest, 'w', sizeof(p->echorest) - 1);
			strcpy(p->echorest + (p->echotraining / 400) + 1, p->dop.dialstr + strlen(p->dop.dialstr) - 2);
			p->echorest[sizeof(p->echorest) - 1] = '\0';
			p->echobreak = 1;
			p->dop.dialstr[strlen(p->dop.dialstr)-2] = '\0';
		} else
			p->echobreak = 0;
		if (!res) {
			if (ioctl(p->subs[SUB_REAL].dfd, DAHDI_DIAL, &p->dop)) {
				int saveerr = errno;

				x = DAHDI_ONHOOK;
				ioctl(p->subs[SUB_REAL].dfd, DAHDI_HOOK, &x);
				ast_log(LOG_WARNING, "Dialing failed on channel %d: %s\n", p->channel, strerror(saveerr));
				ast_mutex_unlock(&p->lock);
				return -1;
			}
		} else
			ast_debug(1, "Deferring dialing...\n");

		p->dialing = 1;
		if (ast_strlen_zero(c))
			p->dialednone = 1;
		ast_setstate(ast, AST_STATE_DIALING);
		break;
	case 0:
		/* Special pseudo -- automatically up*/
		ast_setstate(ast, AST_STATE_UP);
		break;
	case SIG_PRI:
	case SIG_BRI:
	case SIG_BRI_PTMP:
	case SIG_SS7:
		/* We'll get it in a moment -- but use dialdest to store pre-setup_ack digits */
		p->dialdest[0] = '\0';
		p->dialing = 1;
		break;
	default:
		ast_debug(1, "not yet implemented\n");
		ast_mutex_unlock(&p->lock);
		return -1;
	}
#ifdef HAVE_SS7
	if (p->ss7) {
		char ss7_called_nai;
		int called_nai_strip;
		char ss7_calling_nai;
		int calling_nai_strip;
		char ss7_orig_called_nai;
		int orig_called_num_nai_strip;
		char ss7_redirecting_num_nai;
		int redirecting_num_nai_strip;
		unsigned char redirect_info_ind_num;
		unsigned char redirect_info_orig_reas_num;
		unsigned char redirect_info_counter_num;
		unsigned char redirect_info_reas_num;
		unsigned char ss7_cug_indicator;
		const char *ss7_redirect_counter;
		const char *charge_str = NULL;
		const char *gen_address = NULL;
		const char *gen_digits = NULL;
		const char *gen_dig_type = NULL;
		const char *gen_dig_scheme = NULL;
		const char *gen_name = NULL;
		const char *jip_digits = NULL;
		const char *lspi_ident = NULL;
		const char *rlt_flag = NULL;
		const char *call_ref_id = NULL;
		const char *call_ref_pc = NULL;
		const char *send_far = NULL;
		const char *redirecting_number = NULL;
		const char *ss7_orig_called_num = NULL;
		const char *redirect_info_ind = NULL;
		const char *redirect_info_orig_reas = NULL;
		const char *redirect_info_counter = NULL;
		const char *redirect_info_reas = NULL;
		const char *col_req = NULL;
		const char *ss7_cug_indicator_str;
		const char *ss7_cug_interlock_ni;
		const char *ss7_cug_interlock_code;

		c = strchr(dest, '/');
		if (c)
			c++;
		else
			c = dest;

		if (!p->hidecallerid) {
			l = ast->cid.cid_num;
		} else {
			l = NULL;
		}

		if (ss7_grab(p, p->ss7)) {
			ast_log(LOG_WARNING, "Failed to grab SS7!\n");
			ast_mutex_unlock(&p->lock);
			return -1;
		}
		p->digital = IS_DIGITAL(ast->transfercapability);
		p->ss7call = isup_new_call(p->ss7->ss7);

		if (!p->ss7call) {
			ss7_rel(p->ss7);
			ast_mutex_unlock(&p->lock);
			ast_log(LOG_ERROR, "Unable to allocate new SS7 call!\n");
			return -1;
		}

		called_nai_strip = 0;
 		ss7_called_nai = p->ss7->called_nai;

		if (ss7_called_nai == SS7_NAI_DYNAMIC) /* compute dynamically */
			called_nai_strip = ss7_parse_prefix(p, c + p->stripmsd, &ss7_called_nai);

 		isup_set_called(p->ss7call, c + p->stripmsd + called_nai_strip, ss7_called_nai, p->ss7->ss7);

		calling_nai_strip = 0;
		ss7_calling_nai = p->ss7->calling_nai;

		if (l && ss7_calling_nai == SS7_NAI_DYNAMIC) /* compute dynamically */
			calling_nai_strip = ss7_parse_prefix(p, l, &ss7_calling_nai);

		if(ast->cid.cid_pres == AST_PRES_UNAVAILABLE)
			isup_set_calling(p->ss7call, NULL, 0, SS7_PRESENTATION_ADDR_NOT_AVAILABLE, 3);
		else
			isup_set_calling(p->ss7call, l ? (l + calling_nai_strip) : NULL, ss7_calling_nai,
				p->use_callingpres ? cid_pres2ss7pres(ast->cid.cid_pres) : (l ? SS7_PRESENTATION_ALLOWED : SS7_PRESENTATION_RESTRICTED),
				p->use_callingpres ? cid_pres2ss7screen(ast->cid.cid_pres) : SS7_SCREENING_USER_PROVIDED );

		isup_set_oli(p->ss7call, ast->cid.cid_ani2);
		isup_init_call(p->ss7->ss7, p->ss7call, p->cic, p->dpc);

		ast_channel_lock(ast);
		/* Set the charge number if it is set */
		charge_str = pbx_builtin_getvar_helper(ast, "SS7_CHARGE_NUMBER");
		if (charge_str)
			isup_set_charge(p->ss7call, charge_str, SS7_ANI_CALLING_PARTY_SUB_NUMBER, 0x10);

		gen_address = pbx_builtin_getvar_helper(ast, "SS7_GENERIC_ADDRESS");
		if (gen_address)
			isup_set_gen_address(p->ss7call, gen_address, p->gen_add_nai,p->gen_add_pres_ind, p->gen_add_num_plan,p->gen_add_type); /* need to add some types here for NAI,PRES,TYPE */

		gen_digits = pbx_builtin_getvar_helper(ast, "SS7_GENERIC_DIGITS");
		gen_dig_type = pbx_builtin_getvar_helper(ast, "SS7_GENERIC_DIGTYPE");
		gen_dig_scheme = pbx_builtin_getvar_helper(ast, "SS7_GENERIC_DIGSCHEME");
		if (gen_digits)
			isup_set_gen_digits(p->ss7call, gen_digits, atoi(gen_dig_type), atoi(gen_dig_scheme));

		gen_name = pbx_builtin_getvar_helper(ast, "SS7_GENERIC_NAME");
		if (gen_name)
			isup_set_generic_name(p->ss7call, gen_name, GEN_NAME_TYPE_CALLING_NAME, GEN_NAME_AVAIL_AVAILABLE, GEN_NAME_PRES_ALLOWED);

		jip_digits = pbx_builtin_getvar_helper(ast, "SS7_JIP");
		if (jip_digits)
			isup_set_jip_digits(p->ss7call, jip_digits);

		lspi_ident = pbx_builtin_getvar_helper(ast, "SS7_LSPI_IDENT");
		if (lspi_ident)
			isup_set_lspi(p->ss7call, lspi_ident, 0x18, 0x7, 0x00);

		rlt_flag = pbx_builtin_getvar_helper(ast, "SS7_RLT_ON");
		if ((rlt_flag) && ((strncmp("NO", rlt_flag, strlen(rlt_flag))) != 0 )) {
			isup_set_lspi(p->ss7call, rlt_flag, 0x18, 0x7, 0x00); /* Setting for Nortel DMS-250/500 */
		}

		call_ref_id = pbx_builtin_getvar_helper(ast, "SS7_CALLREF_IDENT");
		call_ref_pc = pbx_builtin_getvar_helper(ast, "SS7_CALLREF_PC");
		if (call_ref_id && call_ref_pc) {
			isup_set_callref(p->ss7call, atoi(call_ref_id),
					 call_ref_pc ? atoi(call_ref_pc) : 0);
		}

		send_far = pbx_builtin_getvar_helper(ast, "SS7_SEND_FAR");
		if ((send_far) && ((strncmp("NO", send_far, strlen(send_far))) != 0 ))
			(isup_far(p->ss7->ss7, p->ss7call));

		redirecting_number = pbx_builtin_getvar_helper(ast, "SS7_REDIRECTING_NUMBER");

		if(redirecting_number) {
			redirecting_num_nai_strip = 0;
			ss7_redirecting_num_nai = p->ss7->calling_nai;
			if (ss7_redirecting_num_nai == SS7_NAI_DYNAMIC)
				redirecting_num_nai_strip = ss7_parse_prefix(p, redirecting_number, &ss7_redirecting_num_nai);
			isup_set_redirecting_number(p->ss7call, redirecting_number + redirecting_num_nai_strip, ss7_redirecting_num_nai, SS7_PRESENTATION_RESTRICTED, 3);
		}

		ss7_redirect_counter = pbx_builtin_getvar_helper(ast, "SS7_REDIRECT_COUNTER");
		if (ss7_redirect_counter)
			isup_set_redirect_counter(p->ss7call, atoi(ss7_redirect_counter));

		ss7_orig_called_num = pbx_builtin_getvar_helper(ast, "SS7_ORIG_CALLED_NUM");
		if (ss7_orig_called_num) {
			orig_called_num_nai_strip = 0;
			ss7_orig_called_nai = p->ss7->called_nai;
			if (ss7_orig_called_nai == SS7_NAI_DYNAMIC)
				orig_called_num_nai_strip = ss7_parse_prefix(p, ss7_orig_called_num, &ss7_orig_called_nai);
			isup_set_orig_called_num(p->ss7call, ss7_orig_called_num + orig_called_num_nai_strip, ss7_orig_called_nai,
				SS7_PRESENTATION_RESTRICTED, 3);
		}

		redirect_info_ind = pbx_builtin_getvar_helper(ast, "SS7_REDIRECT_INFO_IND");
		if (redirect_info_ind && redirect_info_ind[0] != '\0') {
			if(!strcasecmp(redirect_info_ind, "NO_REDIRECTION"))
				redirect_info_ind_num = 0;
			else if(!strcasecmp(redirect_info_ind, "CALL_REROUTED_PRES_ALLOWED"))
				redirect_info_ind_num = 1;
			else if(!strcasecmp(redirect_info_ind, "CALL_REROUTED_INFO_RESTRICTED"))
				redirect_info_ind_num = 2;
			else if(!strcasecmp(redirect_info_ind, "CALL_DIVERTED_PRES_ALLOWED"))
				redirect_info_ind_num = 3;
			else if(!strcasecmp(redirect_info_ind, "CALL_DIVERTED_INFO_RESTRICTED"))
				redirect_info_ind_num = 4;
			else if(!strcasecmp(redirect_info_ind, "CALL_REROUTED_PRES_RESTRICTED"))
				redirect_info_ind_num = 5;
			else if(!strcasecmp(redirect_info_ind, "CALL_DIVERTED_PRES_RESTRICTED"))
				redirect_info_ind_num = 6;
			else if(!strcasecmp(redirect_info_ind, "SPARE"))
				redirect_info_ind_num = 7;
			else
				redirect_info_ind_num = 0;

			redirect_info_orig_reas = pbx_builtin_getvar_helper(ast, "SS7_REDIRECT_INFO_ORIG_REAS");
			if (redirect_info_orig_reas) {
				if(!strcasecmp(redirect_info_orig_reas, "UNKNOWN"))
					redirect_info_orig_reas_num = 0;
				else if(!strcasecmp(redirect_info_orig_reas, "USER-BUSY"))
					redirect_info_orig_reas_num = 1;
				else if(!strcasecmp(redirect_info_orig_reas, "NO-ANSWER"))
					redirect_info_orig_reas_num = 2;
				else if(!strcasecmp(redirect_info_orig_reas, "UNCONDITIONAL"))
					redirect_info_orig_reas_num = 3;
				else
					redirect_info_orig_reas_num = 0;
			} else
				redirect_info_orig_reas_num = 0;

			redirect_info_counter = pbx_builtin_getvar_helper(ast, "SS7_REDIRECT_INFO_COUNTER");
			if(redirect_info_counter)
				redirect_info_counter_num = atoi(redirect_info_counter);
			else
				redirect_info_counter_num = 5; /* Avoiding call forwarding loops!!! */

			redirect_info_reas = pbx_builtin_getvar_helper(ast, "SS7_REDIRECT_INFO_REAS");
			if (redirect_info_reas) {
				if(!strcasecmp(redirect_info_reas, "UNKNOWN"))
					redirect_info_reas_num = 0;
				else if(!strcasecmp(redirect_info_reas, "USER-BUSY"))
					redirect_info_reas_num = 1;
				else if(!strcasecmp(redirect_info_reas, "NO-ANSWER"))
					redirect_info_reas_num = 2;
				else if(!strcasecmp(redirect_info_reas, "UNCONDITIONAL"))
					redirect_info_reas_num = 3;
				else if(!strcasecmp(redirect_info_reas, "DEFLECTION_DURING_ALERTING"))
					redirect_info_reas_num = 4;
				else if(!strcasecmp(redirect_info_reas, "DEFLECTION_IMMEDIATE_RESPONSE"))
					redirect_info_reas_num = 5;
				else if(!strcasecmp(redirect_info_reas, "UNAVAILABLE"))
					redirect_info_reas_num = 6;
				else
					redirect_info_reas_num = 0;
			} else
				redirect_info_reas_num = 0;

			isup_set_redirectiong_info(p->ss7call, redirect_info_ind_num, redirect_info_orig_reas_num,
				redirect_info_counter_num, redirect_info_reas_num);
		}

		col_req = pbx_builtin_getvar_helper(ast, "SS7_COL_REQUEST");
		if (ast_true(col_req))
			isup_set_col_req(p->ss7call);

		ss7_cug_indicator_str = pbx_builtin_getvar_helper(ast, "SS7_CUG_INDICATOR");
		if (ss7_cug_indicator_str && ss7_cug_indicator_str[0]) {
			if (!strcasecmp(ss7_cug_indicator_str, "OUTGOING_ALLOWED"))
				ss7_cug_indicator = ISUP_CUG_OUTGOING_ALLOWED;
			else if (!strcasecmp(ss7_cug_indicator_str, "OUTGOING_NOT_ALLOWED"))
				ss7_cug_indicator = ISUP_CUG_OUTGOING_NOT_ALLOWED;
			else
				ss7_cug_indicator = ISUP_CUG_NON;

			if (ss7_cug_indicator != ISUP_CUG_NON) {
				ss7_cug_interlock_code = pbx_builtin_getvar_helper(ast, "SS7_CUG_INTERLOCK_CODE");
				ss7_cug_interlock_ni = pbx_builtin_getvar_helper(ast, "SS7_CUG_INTERLOCK_NI");
				if (ss7_cug_interlock_code && ss7_cug_interlock_ni && strlen(ss7_cug_interlock_ni) == 4)
					isup_set_cug(p->ss7call, ss7_cug_indicator, ss7_cug_interlock_ni, atoi(ss7_cug_interlock_code));
			}
		}

		isup_set_echocontrol(p->ss7call, (p->ss7->flags & LINKSET_FLAG_DEFAULTECHOCONTROL) ? 1 : 0);

		ast_channel_unlock(ast);

		isup_iam(p->ss7->ss7, p->ss7call);
		ast_setstate(ast, AST_STATE_DIALING);
		ss7_rel(p->ss7);
	}
#endif /* HAVE_SS7 */
#ifdef HAVE_PRI
	if (p->pri) {
		struct pri_sr *sr;
#ifdef SUPPORT_USERUSER
		const char *useruser;
#endif
		int pridialplan;
		int dp_strip;
		int prilocaldialplan;
		int ldp_strip;
		int exclusive;
		const char *rr_str;
		int redirect_reason;

		c = strchr(dest, '/');
		if (c)
			c++;
		else
			c = dest;

		l = NULL;
		n = NULL;

		if (!p->hidecallerid) {
			l = ast->cid.cid_num;
			if (!p->hidecalleridname) {
				n = ast->cid.cid_name;
			}
		}

		if (strlen(c) < p->stripmsd) {
			ast_log(LOG_WARNING, "Number '%s' is shorter than stripmsd (%d)\n", c, p->stripmsd);
			ast_mutex_unlock(&p->lock);
			return -1;
		}
		if (mysig != SIG_FXSKS) {
			p->dop.op = DAHDI_DIAL_OP_REPLACE;
			s = strchr(c + p->stripmsd, 'w');
			if (s) {
				if (strlen(s) > 1)
					snprintf(p->dop.dialstr, sizeof(p->dop.dialstr), "T%s", s);
				else
					p->dop.dialstr[0] = '\0';
				*s = '\0';
			} else {
				p->dop.dialstr[0] = '\0';
			}
		}
		if (pri_grab(p, p->pri)) {
			ast_log(LOG_WARNING, "Failed to grab PRI!\n");
			ast_mutex_unlock(&p->lock);
			return -1;
		}
		if (!(p->call = pri_new_call(p->pri->pri))) {
			ast_log(LOG_WARNING, "Unable to create call on channel %d\n", p->channel);
			pri_rel(p->pri);
			ast_mutex_unlock(&p->lock);
			return -1;
		}
		if (!(sr = pri_sr_new())) {
			ast_log(LOG_WARNING, "Failed to allocate setup request channel %d\n", p->channel);
			pri_rel(p->pri);
			ast_mutex_unlock(&p->lock);
		}
		if (p->bearer || (mysig == SIG_FXSKS)) {
			if (p->bearer) {
				ast_debug(1, "Oooh, I have a bearer on %d (%d:%d)\n", PVT_TO_CHANNEL(p->bearer), p->bearer->logicalspan, p->bearer->channel);
				p->bearer->call = p->call;
			} else
				ast_debug(1, "I'm being setup with no bearer right now...\n");

			pri_set_crv(p->pri->pri, p->call, p->channel, 0);
		}
		p->digital = IS_DIGITAL(ast->transfercapability);
		/* Add support for exclusive override */
		if (p->priexclusive)
			exclusive = 1;
		else {
		/* otherwise, traditional behavior */
			if (p->pri->nodetype == PRI_NETWORK)
				exclusive = 0;
			else
				exclusive = 1;
		}

		pri_sr_set_channel(sr, p->bearer ? PVT_TO_CHANNEL(p->bearer) : PVT_TO_CHANNEL(p), exclusive, 1);
		pri_sr_set_bearer(sr, p->digital ? PRI_TRANS_CAP_DIGITAL : ast->transfercapability,
					(p->digital ? -1 :
						((p->law == DAHDI_LAW_ALAW) ? PRI_LAYER_1_ALAW : PRI_LAYER_1_ULAW)));
		if (p->pri->facilityenable)
			pri_facility_enable(p->pri->pri);

		ast_verb(3, "Requested transfer capability: 0x%.2x - %s\n", ast->transfercapability, ast_transfercapability2str(ast->transfercapability));
		dp_strip = 0;
 		pridialplan = p->pri->dialplan - 1;
		if (pridialplan == -2 || pridialplan == -3) { /* compute dynamically */
 			if (strncmp(c + p->stripmsd, p->pri->internationalprefix, strlen(p->pri->internationalprefix)) == 0) {
				if (pridialplan == -2) {
 					dp_strip = strlen(p->pri->internationalprefix);
				}
 				pridialplan = PRI_INTERNATIONAL_ISDN;
 			} else if (strncmp(c + p->stripmsd, p->pri->nationalprefix, strlen(p->pri->nationalprefix)) == 0) {
				if (pridialplan == -2) {
 					dp_strip = strlen(p->pri->nationalprefix);
				}
 				pridialplan = PRI_NATIONAL_ISDN;
 			} else {
				pridialplan = PRI_LOCAL_ISDN;
 			}
 		}
		while (c[p->stripmsd] > '9' && c[p->stripmsd] != '*' && c[p->stripmsd] != '#') {
			switch (c[p->stripmsd]) {
			case 'U':
				pridialplan = (PRI_TON_UNKNOWN << 4) | (pridialplan & 0xf);
				break;
			case 'I':
				pridialplan = (PRI_TON_INTERNATIONAL << 4) | (pridialplan & 0xf);
				break;
			case 'N':
				pridialplan = (PRI_TON_NATIONAL << 4) | (pridialplan & 0xf);
				break;
			case 'L':
				pridialplan = (PRI_TON_NET_SPECIFIC << 4) | (pridialplan & 0xf);
				break;
			case 'S':
				pridialplan = (PRI_TON_SUBSCRIBER << 4) | (pridialplan & 0xf);
				break;
			case 'V':
				pridialplan = (PRI_TON_ABBREVIATED << 4) | (pridialplan & 0xf);
				break;
			case 'R':
				pridialplan = (PRI_TON_RESERVED << 4) | (pridialplan & 0xf);
				break;
			case 'u':
				pridialplan = PRI_NPI_UNKNOWN | (pridialplan & 0xf0);
				break;
			case 'e':
				pridialplan = PRI_NPI_E163_E164 | (pridialplan & 0xf0);
				break;
			case 'x':
				pridialplan = PRI_NPI_X121 | (pridialplan & 0xf0);
				break;
			case 'f':
				pridialplan = PRI_NPI_F69 | (pridialplan & 0xf0);
				break;
			case 'n':
				pridialplan = PRI_NPI_NATIONAL | (pridialplan & 0xf0);
				break;
			case 'p':
				pridialplan = PRI_NPI_PRIVATE | (pridialplan & 0xf0);
				break;
			case 'r':
				pridialplan = PRI_NPI_RESERVED | (pridialplan & 0xf0);
				break;
			default:
				if (isalpha(*c))
					ast_log(LOG_WARNING, "Unrecognized pridialplan %s modifier: %c\n", *c > 'Z' ? "NPI" : "TON", *c);
			}
			c++;
		}
 		pri_sr_set_called(sr, c + p->stripmsd + dp_strip, pridialplan, s ? 1 : 0);

		ldp_strip = 0;
		prilocaldialplan = p->pri->localdialplan - 1;
		if ((l != NULL) && (prilocaldialplan == -2 || prilocaldialplan == -3)) { /* compute dynamically */
			if (strncmp(l, p->pri->internationalprefix, strlen(p->pri->internationalprefix)) == 0) {
				if (prilocaldialplan == -2) {
					ldp_strip = strlen(p->pri->internationalprefix);
				}
				prilocaldialplan = PRI_INTERNATIONAL_ISDN;
			} else if (strncmp(l, p->pri->nationalprefix, strlen(p->pri->nationalprefix)) == 0) {
				if (prilocaldialplan == -2) {
					ldp_strip = strlen(p->pri->nationalprefix);
				}
				prilocaldialplan = PRI_NATIONAL_ISDN;
			} else {
				prilocaldialplan = PRI_LOCAL_ISDN;
			}
		}
		if (l != NULL) {
			while (*l > '9' && *l != '*' && *l != '#') {
				switch (*l) {
				case 'U':
					prilocaldialplan = (PRI_TON_UNKNOWN << 4) | (prilocaldialplan & 0xf);
					break;
				case 'I':
					prilocaldialplan = (PRI_TON_INTERNATIONAL << 4) | (prilocaldialplan & 0xf);
					break;
				case 'N':
					prilocaldialplan = (PRI_TON_NATIONAL << 4) | (prilocaldialplan & 0xf);
					break;
				case 'L':
					prilocaldialplan = (PRI_TON_NET_SPECIFIC << 4) | (prilocaldialplan & 0xf);
					break;
				case 'S':
					prilocaldialplan = (PRI_TON_SUBSCRIBER << 4) | (prilocaldialplan & 0xf);
					break;
				case 'V':
					prilocaldialplan = (PRI_TON_ABBREVIATED << 4) | (prilocaldialplan & 0xf);
					break;
				case 'R':
					prilocaldialplan = (PRI_TON_RESERVED << 4) | (prilocaldialplan & 0xf);
					break;
				case 'u':
					prilocaldialplan = PRI_NPI_UNKNOWN | (prilocaldialplan & 0xf0);
					break;
				case 'e':
					prilocaldialplan = PRI_NPI_E163_E164 | (prilocaldialplan & 0xf0);
					break;
				case 'x':
					prilocaldialplan = PRI_NPI_X121 | (prilocaldialplan & 0xf0);
					break;
				case 'f':
					prilocaldialplan = PRI_NPI_F69 | (prilocaldialplan & 0xf0);
					break;
				case 'n':
					prilocaldialplan = PRI_NPI_NATIONAL | (prilocaldialplan & 0xf0);
					break;
				case 'p':
					prilocaldialplan = PRI_NPI_PRIVATE | (prilocaldialplan & 0xf0);
					break;
				case 'r':
					prilocaldialplan = PRI_NPI_RESERVED | (prilocaldialplan & 0xf0);
					break;
				default:
					if (isalpha(*l))
						ast_log(LOG_WARNING, "Unrecognized prilocaldialplan %s modifier: %c\n", *c > 'Z' ? "NPI" : "TON", *c);
				}
				l++;
			}
		}
		pri_sr_set_caller(sr, l ? (l + ldp_strip) : NULL, n, prilocaldialplan,
			p->use_callingpres ? ast->cid.cid_pres : (l ? PRES_ALLOWED_USER_NUMBER_PASSED_SCREEN : PRES_NUMBER_NOT_AVAILABLE));
		if ((rr_str = pbx_builtin_getvar_helper(ast, "PRIREDIRECTREASON"))) {
			if (!strcasecmp(rr_str, "UNKNOWN"))
				redirect_reason = 0;
			else if (!strcasecmp(rr_str, "BUSY"))
				redirect_reason = 1;
			else if (!strcasecmp(rr_str, "NO_REPLY"))
				redirect_reason = 2;
			else if (!strcasecmp(rr_str, "UNCONDITIONAL"))
				redirect_reason = 15;
			else
				redirect_reason = PRI_REDIR_UNCONDITIONAL;
		} else
			redirect_reason = PRI_REDIR_UNCONDITIONAL;
		pri_sr_set_redirecting(sr, ast->cid.cid_rdnis, p->pri->localdialplan - 1, PRES_ALLOWED_USER_NUMBER_PASSED_SCREEN, redirect_reason);

#ifdef SUPPORT_USERUSER
		/* User-user info */
		useruser = pbx_builtin_getvar_helper(p->owner, "USERUSERINFO");

		if (useruser)
			pri_sr_set_useruser(sr, useruser);
#endif

		if (pri_setup(p->pri->pri, p->call, sr)) {
 			ast_log(LOG_WARNING, "Unable to setup call to %s (using %s)\n",
 				c + p->stripmsd + dp_strip, dialplan2str(p->pri->dialplan));
			pri_rel(p->pri);
			ast_mutex_unlock(&p->lock);
			pri_sr_free(sr);
			return -1;
		}
		pri_sr_free(sr);
		ast_setstate(ast, AST_STATE_DIALING);
		pri_rel(p->pri);
	}
#endif
	ast_mutex_unlock(&p->lock);
	return 0;
}

static void destroy_dahdi_pvt(struct dahdi_pvt **pvt)
{
	struct dahdi_pvt *p = *pvt;
	/* Remove channel from the list */
	if (p->prev)
		p->prev->next = p->next;
	if (p->next)
		p->next->prev = p->prev;
	if (p->use_smdi)
		ast_smdi_interface_unref(p->smdi_iface);
	if (p->mwi_event_sub)
		ast_event_unsubscribe(p->mwi_event_sub);
	if (p->vars)
		ast_variables_destroy(p->vars);
	ast_mutex_destroy(&p->lock);
	if (p->owner)
		p->owner->tech_pvt = NULL;
	free(p);
	*pvt = NULL;
}

static int destroy_channel(struct dahdi_pvt *prev, struct dahdi_pvt *cur, int now)
{
	int owned = 0;
	int i = 0;

	if (!now) {
		if (cur->owner) {
			owned = 1;
		}

		for (i = 0; i < 3; i++) {
			if (cur->subs[i].owner) {
				owned = 1;
			}
		}
		if (!owned) {
			if (prev) {
				prev->next = cur->next;
				if (prev->next)
					prev->next->prev = prev;
				else
					ifend = prev;
			} else {
				iflist = cur->next;
				if (iflist)
					iflist->prev = NULL;
				else
					ifend = NULL;
			}
			if (cur->subs[SUB_REAL].dfd > -1) {
				dahdi_close(cur->subs[SUB_REAL].dfd);
			}
			destroy_dahdi_pvt(&cur);
		}
	} else {
		if (prev) {
			prev->next = cur->next;
			if (prev->next)
				prev->next->prev = prev;
			else
				ifend = prev;
		} else {
			iflist = cur->next;
			if (iflist)
				iflist->prev = NULL;
			else
				ifend = NULL;
		}
		if (cur->subs[SUB_REAL].dfd > -1) {
			dahdi_close(cur->subs[SUB_REAL].dfd);
		}
		destroy_dahdi_pvt(&cur);
	}
	return 0;
}

static void destroy_all_channels(void)
{
	int x;
	struct dahdi_pvt *p, *pl;

	while (num_restart_pending) {
		usleep(1);
	}

	ast_mutex_lock(&iflock);
	/* Destroy all the interfaces and free their memory */
	p = iflist;
	while (p) {
		/* Free any callerid */
		if (p->cidspill)
			ast_free(p->cidspill);
		/* Close the DAHDI thingy */
		if (p->subs[SUB_REAL].dfd > -1)
			dahdi_close(p->subs[SUB_REAL].dfd);
		pl = p;
		p = p->next;
		x = pl->channel;
		/* Free associated memory */
		if (pl)
			destroy_dahdi_pvt(&pl);
		if (option_verbose > 2)
			ast_verbose(VERBOSE_PREFIX_2 "Unregistered channel %d\n", x);
	}
	iflist = NULL;
	ifcount = 0;
	ast_mutex_unlock(&iflock);
}

#ifdef HAVE_PRI
static char *dahdi_send_keypad_facility_app = "DAHDISendKeypadFacility";

static char *dahdi_send_keypad_facility_synopsis = "Send digits out of band over a PRI";

static char *dahdi_send_keypad_facility_descrip =
"  DAHDISendKeypadFacility(): This application will send the given string of digits in a Keypad Facility\n"
"  IE over the current channel.\n";

static int dahdi_send_keypad_facility_exec(struct ast_channel *chan, void *data)
{
	/* Data will be our digit string */
	struct dahdi_pvt *p;
	char *digits = (char *) data;

	if (ast_strlen_zero(digits)) {
		ast_debug(1, "No digit string sent to application!\n");
		return -1;
	}

	p = (struct dahdi_pvt *)chan->tech_pvt;

	if (!p) {
		ast_debug(1, "Unable to find technology private\n");
		return -1;
	}

	ast_mutex_lock(&p->lock);

	if (!p->pri || !p->call) {
		ast_debug(1, "Unable to find pri or call on channel!\n");
		ast_mutex_unlock(&p->lock);
		return -1;
	}

	if (!pri_grab(p, p->pri)) {
		pri_keypad_facility(p->pri->pri, p->call, digits);
		pri_rel(p->pri);
	} else {
		ast_debug(1, "Unable to grab pri to send keypad facility!\n");
		ast_mutex_unlock(&p->lock);
		return -1;
	}

	ast_mutex_unlock(&p->lock);

	return 0;
}

static int pri_is_up(struct dahdi_pri *pri)
{
	int x;
	for (x = 0; x < NUM_DCHANS; x++) {
		if (pri->dchanavail[x] == DCHAN_AVAILABLE)
			return 1;
	}
	return 0;
}

static int pri_assign_bearer(struct dahdi_pvt *crv, struct dahdi_pri *pri, struct dahdi_pvt *bearer)
{
	bearer->owner = &inuse;
	bearer->realcall = crv;
	crv->subs[SUB_REAL].dfd = bearer->subs[SUB_REAL].dfd;
	if (crv->subs[SUB_REAL].owner)
		ast_channel_set_fd(crv->subs[SUB_REAL].owner, 0, crv->subs[SUB_REAL].dfd);
	crv->bearer = bearer;
	crv->call = bearer->call;
	crv->pri = pri;
	return 0;
}

static char *pri_order(int level)
{
	switch (level) {
	case 0:
		return "Primary";
	case 1:
		return "Secondary";
	case 2:
		return "Tertiary";
	case 3:
		return "Quaternary";
	default:
		return "<Unknown>";
	}
}

/* Returns fd of the active dchan */
static int pri_active_dchan_fd(struct dahdi_pri *pri)
{
	int x = -1;

	for (x = 0; x < NUM_DCHANS; x++) {
		if ((pri->dchans[x] == pri->pri))
			break;
	}

	return pri->fds[x];
}

static int pri_find_dchan(struct dahdi_pri *pri)
{
	int oldslot = -1;
	struct pri *old;
	int newslot = -1;
	int x;
	old = pri->pri;
	for (x = 0; x < NUM_DCHANS; x++) {
		if ((pri->dchanavail[x] == DCHAN_AVAILABLE) && (newslot < 0))
			newslot = x;
		if (pri->dchans[x] == old) {
			oldslot = x;
		}
	}
	if (newslot < 0) {
		newslot = 0;
		ast_log(LOG_WARNING, "No D-channels available!  Using Primary channel %d as D-channel anyway!\n",
			pri->dchannels[newslot]);
	}
	if (old && (oldslot != newslot))
		ast_log(LOG_NOTICE, "Switching from from d-channel %d to channel %d!\n",
			pri->dchannels[oldslot], pri->dchannels[newslot]);
	pri->pri = pri->dchans[newslot];
	return 0;
}
#endif

static int dahdi_hangup(struct ast_channel *ast)
{
	int res;
	int index,x, law;
	/*static int restore_gains(struct dahdi_pvt *p);*/
	struct dahdi_pvt *p = ast->tech_pvt;
	struct dahdi_pvt *tmp = NULL;
	struct dahdi_pvt *prev = NULL;
	struct dahdi_params par;

	ast_debug(1, "dahdi_hangup(%s)\n", ast->name);
	if (!ast->tech_pvt) {
		ast_log(LOG_WARNING, "Asked to hangup channel not connected\n");
		return 0;
	}

	ast_mutex_lock(&p->lock);

	index = dahdi_get_index(ast, p, 1);

	if ((p->sig == SIG_PRI) || (p->sig == SIG_SS7) || (p->sig == SIG_BRI) || (p->sig == SIG_BRI_PTMP)) {
		x = 1;
		ast_channel_setoption(ast,AST_OPTION_AUDIO_MODE,&x,sizeof(char),0);
	}

	x = 0;
	dahdi_confmute(p, 0);
	restore_gains(p);
	if (p->origcid_num) {
		ast_copy_string(p->cid_num, p->origcid_num, sizeof(p->cid_num));
		ast_free(p->origcid_num);
		p->origcid_num = NULL;
	}
	if (p->origcid_name) {
		ast_copy_string(p->cid_name, p->origcid_name, sizeof(p->cid_name));
		ast_free(p->origcid_name);
		p->origcid_name = NULL;
	}
	if (p->dsp)
		ast_dsp_digitmode(p->dsp, DSP_DIGITMODE_DTMF | p->dtmfrelax);
	if (p->exten)
		p->exten[0] = '\0';

	ast_debug(1, "Hangup: channel: %d index = %d, normal = %d, callwait = %d, thirdcall = %d\n",
		p->channel, index, p->subs[SUB_REAL].dfd, p->subs[SUB_CALLWAIT].dfd, p->subs[SUB_THREEWAY].dfd);
	p->ignoredtmf = 0;

	if (index > -1) {
		/* Real channel, do some fixup */
		p->subs[index].owner = NULL;
		p->subs[index].needanswer = 0;
		p->subs[index].needflash = 0;
		p->subs[index].needringing = 0;
		p->subs[index].needbusy = 0;
		p->subs[index].needcongestion = 0;
		p->subs[index].linear = 0;
		p->subs[index].needcallerid = 0;
		p->polarity = POLARITY_IDLE;
		dahdi_setlinear(p->subs[index].dfd, 0);
		if (index == SUB_REAL) {
			if ((p->subs[SUB_CALLWAIT].dfd > -1) && (p->subs[SUB_THREEWAY].dfd > -1)) {
				ast_debug(1, "Normal call hung up with both three way call and a call waiting call in place?\n");
				if (p->subs[SUB_CALLWAIT].inthreeway) {
					/* We had flipped over to answer a callwait and now it's gone */
					ast_debug(1, "We were flipped over to the callwait, moving back and unowning.\n");
					/* Move to the call-wait, but un-own us until they flip back. */
					swap_subs(p, SUB_CALLWAIT, SUB_REAL);
					unalloc_sub(p, SUB_CALLWAIT);
					p->owner = NULL;
				} else {
					/* The three way hung up, but we still have a call wait */
					ast_debug(1, "We were in the threeway and have a callwait still.  Ditching the threeway.\n");
					swap_subs(p, SUB_THREEWAY, SUB_REAL);
					unalloc_sub(p, SUB_THREEWAY);
					if (p->subs[SUB_REAL].inthreeway) {
						/* This was part of a three way call.  Immediately make way for
						   another call */
						ast_debug(1, "Call was complete, setting owner to former third call\n");
						p->owner = p->subs[SUB_REAL].owner;
					} else {
						/* This call hasn't been completed yet...  Set owner to NULL */
						ast_debug(1, "Call was incomplete, setting owner to NULL\n");
						p->owner = NULL;
					}
					p->subs[SUB_REAL].inthreeway = 0;
				}
			} else if (p->subs[SUB_CALLWAIT].dfd > -1) {
				/* Move to the call-wait and switch back to them. */
				swap_subs(p, SUB_CALLWAIT, SUB_REAL);
				unalloc_sub(p, SUB_CALLWAIT);
				p->owner = p->subs[SUB_REAL].owner;
				if (p->owner->_state != AST_STATE_UP)
					p->subs[SUB_REAL].needanswer = 1;
				if (ast_bridged_channel(p->subs[SUB_REAL].owner))
					ast_queue_control(p->subs[SUB_REAL].owner, AST_CONTROL_UNHOLD);
			} else if (p->subs[SUB_THREEWAY].dfd > -1) {
				swap_subs(p, SUB_THREEWAY, SUB_REAL);
				unalloc_sub(p, SUB_THREEWAY);
				if (p->subs[SUB_REAL].inthreeway) {
					/* This was part of a three way call.  Immediately make way for
					   another call */
					ast_debug(1, "Call was complete, setting owner to former third call\n");
					p->owner = p->subs[SUB_REAL].owner;
				} else {
					/* This call hasn't been completed yet...  Set owner to NULL */
					ast_debug(1, "Call was incomplete, setting owner to NULL\n");
					p->owner = NULL;
				}
				p->subs[SUB_REAL].inthreeway = 0;
			}
		} else if (index == SUB_CALLWAIT) {
			/* Ditch the holding callwait call, and immediately make it availabe */
			if (p->subs[SUB_CALLWAIT].inthreeway) {
				/* This is actually part of a three way, placed on hold.  Place the third part
				   on music on hold now */
				if (p->subs[SUB_THREEWAY].owner && ast_bridged_channel(p->subs[SUB_THREEWAY].owner)) {
					ast_queue_control_data(p->subs[SUB_THREEWAY].owner, AST_CONTROL_HOLD,
						S_OR(p->mohsuggest, NULL),
						!ast_strlen_zero(p->mohsuggest) ? strlen(p->mohsuggest) + 1 : 0);
				}
				p->subs[SUB_THREEWAY].inthreeway = 0;
				/* Make it the call wait now */
				swap_subs(p, SUB_CALLWAIT, SUB_THREEWAY);
				unalloc_sub(p, SUB_THREEWAY);
			} else
				unalloc_sub(p, SUB_CALLWAIT);
		} else if (index == SUB_THREEWAY) {
			if (p->subs[SUB_CALLWAIT].inthreeway) {
				/* The other party of the three way call is currently in a call-wait state.
				   Start music on hold for them, and take the main guy out of the third call */
				if (p->subs[SUB_CALLWAIT].owner && ast_bridged_channel(p->subs[SUB_CALLWAIT].owner)) {
					ast_queue_control_data(p->subs[SUB_CALLWAIT].owner, AST_CONTROL_HOLD,
						S_OR(p->mohsuggest, NULL),
						!ast_strlen_zero(p->mohsuggest) ? strlen(p->mohsuggest) + 1 : 0);
				}
				p->subs[SUB_CALLWAIT].inthreeway = 0;
			}
			p->subs[SUB_REAL].inthreeway = 0;
			/* If this was part of a three way call index, let us make
			   another three way call */
			unalloc_sub(p, SUB_THREEWAY);
		} else {
			/* This wasn't any sort of call, but how are we an index? */
			ast_log(LOG_WARNING, "Index found but not any type of call?\n");
		}
	}

	if (!p->subs[SUB_REAL].owner && !p->subs[SUB_CALLWAIT].owner && !p->subs[SUB_THREEWAY].owner) {
		p->owner = NULL;
		p->ringt = 0;
		p->distinctivering = 0;
		p->confirmanswer = 0;
		p->cidrings = 1;
		p->outgoing = 0;
		p->digital = 0;
		p->faxhandled = 0;
		p->pulsedial = 0;
		p->onhooktime = time(NULL);
#if defined(HAVE_PRI) || defined(HAVE_SS7)
		p->proceeding = 0;
		p->dialing = 0;
		p->progress = 0;
		p->alerting = 0;
		p->setup_ack = 0;
		p->rlt = 0;
#endif
		if (p->dsp) {
			ast_dsp_free(p->dsp);
			p->dsp = NULL;
		}

		law = DAHDI_LAW_DEFAULT;
		res = ioctl(p->subs[SUB_REAL].dfd, DAHDI_SETLAW, &law);
		if (res < 0)
			ast_log(LOG_WARNING, "Unable to set law on channel %d to default: %s\n", p->channel, strerror(errno));
		/* Perform low level hangup if no owner left */
#ifdef HAVE_SS7
		if (p->ss7) {
			if (p->ss7call) {
				if (!ss7_grab(p, p->ss7)) {
					if (p->do_hangup == SS7_HANGUP_SEND_REL) {
						const char *cause = pbx_builtin_getvar_helper(ast,"SS7_CAUSE");
						int icause = ast->hangupcause ? ast->hangupcause : -1;

						if (cause) {
							if (atoi(cause))
								icause = atoi(cause);
						}
						if(icause > 255)
							icause = 16;

						isup_rel(p->ss7->ss7, p->ss7call, icause);
						p->do_hangup = SS7_HANGUP_DO_NOTHING;
					} else if (p->do_hangup == SS7_HANGUP_SEND_RSC) {
						ss7_do_rsc(p);
						p->do_hangup = SS7_HANGUP_DO_NOTHING;
					} else if (p->do_hangup == SS7_HANGUP_SEND_RLC) {
						isup_rlc(p->ss7->ss7, p->ss7call);
						isup_free_call(p->ss7->ss7, p->ss7call);
						p->ss7call = NULL;
						p->do_hangup = SS7_HANGUP_DO_NOTHING;
					} else if (p->do_hangup == SS7_HANGUP_FREE_CALL) {
						p->do_hangup = SS7_HANGUP_DO_NOTHING;
						isup_free_call(p->ss7->ss7, p->ss7call);
						p->ss7call = NULL;
					} else if (p->do_hangup == SS7_HANGUP_REEVENT_IAM) {
						isup_event_iam(p->ss7->ss7, p->ss7call, p->dpc);
						p->do_hangup = SS7_HANGUP_SEND_REL;
					} else if (p->do_hangup == SS7_HANGUP_DO_NOTHING) {
						p->ss7call = isup_free_call_if_clear(p->ss7->ss7, p->ss7call);
					}
					ss7_rel(p->ss7);
				} else {
					ast_log(LOG_WARNING, "Unable to grab SS7 on CIC %d\n", p->cic);
					res = -1;
				}
			}
		}
#endif
#ifdef HAVE_PRI
		if (p->pri) {
#ifdef SUPPORT_USERUSER
			const char *useruser = pbx_builtin_getvar_helper(ast,"USERUSERINFO");
#endif

			/* Make sure we have a call (or REALLY have a call in the case of a PRI) */
			if (p->call && (!p->bearer || (p->bearer->call == p->call))) {
				if (!pri_grab(p, p->pri)) {
					if (p->alreadyhungup) {
						ast_debug(1, "Already hungup...  Calling hangup once, and clearing call\n");

#ifdef SUPPORT_USERUSER
						pri_call_set_useruser(p->call, useruser);
#endif

						pri_hangup(p->pri->pri, p->call, -1);
						p->call = NULL;
						if (p->bearer)
							p->bearer->call = NULL;
					} else {
						const char *cause = pbx_builtin_getvar_helper(ast,"PRI_CAUSE");
						int icause = ast->hangupcause ? ast->hangupcause : -1;
						ast_debug(1, "Not yet hungup...  Calling hangup once with icause, and clearing call\n");

#ifdef SUPPORT_USERUSER
						pri_call_set_useruser(p->call, useruser);
#endif

						p->alreadyhungup = 1;
						if (p->bearer)
							p->bearer->alreadyhungup = 1;
						if (cause) {
							if (atoi(cause))
								icause = atoi(cause);
						}
						pri_hangup(p->pri->pri, p->call, icause);
					}
					if (res < 0)
						ast_log(LOG_WARNING, "pri_disconnect failed\n");
					pri_rel(p->pri);
				} else {
					ast_log(LOG_WARNING, "Unable to grab PRI on span %d\n", p->span);
					res = -1;
				}
			} else {
				if (p->bearer)
					ast_debug(1, "Bearer call is %p, while ours is still %p\n", p->bearer->call, p->call);
				p->call = NULL;
				res = 0;
			}
		}
#endif
		if (p->sig && ((p->sig != SIG_PRI) && (p->sig != SIG_SS7) && (p->sig != SIG_BRI) && (p->sig != SIG_BRI_PTMP)))
			res = dahdi_set_hook(p->subs[SUB_REAL].dfd, DAHDI_ONHOOK);
		if (res < 0) {
			ast_log(LOG_WARNING, "Unable to hangup line %s\n", ast->name);
		}
		switch (p->sig) {
		case SIG_FXOGS:
		case SIG_FXOLS:
		case SIG_FXOKS:
			memset(&par, 0, sizeof(par));
			res = ioctl(p->subs[SUB_REAL].dfd, DAHDI_GET_PARAMS, &par);
			if (!res) {
#if 0
				ast_debug(1, "Hanging up channel %d, offhook = %d\n", p->channel, par.rxisoffhook);
#endif
				/* If they're off hook, try playing congestion */
				if ((par.rxisoffhook) && (!(p->radio || (p->oprmode < 0))))
					tone_zone_play_tone(p->subs[SUB_REAL].dfd, DAHDI_TONE_CONGESTION);
				else
					tone_zone_play_tone(p->subs[SUB_REAL].dfd, -1);
			}
			break;
		case SIG_FXSGS:
		case SIG_FXSLS:
		case SIG_FXSKS:
			/* Make sure we're not made available for at least two seconds assuming
			   we were actually used for an inbound or outbound call. */
			if (ast->_state != AST_STATE_RESERVED) {
				time(&p->guardtime);
				p->guardtime += 2;
			}
			break;
		default:
			tone_zone_play_tone(p->subs[SUB_REAL].dfd, -1);
		}
		if (p->cidspill)
			ast_free(p->cidspill);
		if (p->sig)
			dahdi_disable_ec(p);
		x = 0;
		ast_channel_setoption(ast,AST_OPTION_TONE_VERIFY,&x,sizeof(char),0);
		ast_channel_setoption(ast,AST_OPTION_TDD,&x,sizeof(char),0);
		p->didtdd = 0;
		p->cidspill = NULL;
		p->callwaitcas = 0;
		p->callwaiting = p->permcallwaiting;
		p->hidecallerid = p->permhidecallerid;
		p->dialing = 0;
		p->rdnis[0] = '\0';
		update_conf(p);
		reset_conf(p);
		/* Restore data mode */
		if ((p->sig == SIG_PRI) || (p->sig == SIG_SS7) || (p->sig == SIG_BRI) || (p->sig == SIG_BRI_PTMP)) {
			x = 0;
			ast_channel_setoption(ast,AST_OPTION_AUDIO_MODE,&x,sizeof(char),0);
		}
#ifdef HAVE_PRI
		if (p->bearer) {
			ast_debug(1, "Freeing up bearer channel %d\n", p->bearer->channel);
			/* Free up the bearer channel as well, and
			   don't use its file descriptor anymore */
			update_conf(p->bearer);
			reset_conf(p->bearer);
			p->bearer->owner = NULL;
			p->bearer->realcall = NULL;
			p->bearer = NULL;
			p->subs[SUB_REAL].dfd = -1;
			p->pri = NULL;
		}
#endif
		if (num_restart_pending == 0)
			restart_monitor();
	}

	p->callwaitingrepeat = 0;
	p->cidcwexpire = 0;
	p->oprmode = 0;
	ast->tech_pvt = NULL;
	ast_mutex_unlock(&p->lock);
	ast_module_unref(ast_module_info->self);
	ast_verb(3, "Hungup '%s'\n", ast->name);

	ast_mutex_lock(&iflock);

	if (p->restartpending) {
		num_restart_pending--;
	}

	tmp = iflist;
	prev = NULL;
	if (p->destroy) {
		while (tmp) {
			if (tmp == p) {
				destroy_channel(prev, tmp, 0);
				break;
			} else {
				prev = tmp;
				tmp = tmp->next;
			}
		}
	}
	ast_mutex_unlock(&iflock);
	return 0;
}

static int dahdi_answer(struct ast_channel *ast)
{
	struct dahdi_pvt *p = ast->tech_pvt;
	int res = 0;
	int index;
	int oldstate = ast->_state;
	ast_setstate(ast, AST_STATE_UP);
	ast_mutex_lock(&p->lock);
	index = dahdi_get_index(ast, p, 0);
	if (index < 0)
		index = SUB_REAL;
	/* nothing to do if a radio channel */
	if ((p->radio || (p->oprmode < 0))) {
		ast_mutex_unlock(&p->lock);
		return 0;
	}
	switch (p->sig) {
	case SIG_FXSLS:
	case SIG_FXSGS:
	case SIG_FXSKS:
		p->ringt = 0;
		/* Fall through */
	case SIG_EM:
	case SIG_EM_E1:
	case SIG_EMWINK:
	case SIG_FEATD:
	case SIG_FEATDMF:
	case SIG_FEATDMF_TA:
	case SIG_E911:
	case SIG_FGC_CAMA:
	case SIG_FGC_CAMAMF:
	case SIG_FEATB:
	case SIG_SF:
	case SIG_SFWINK:
	case SIG_SF_FEATD:
	case SIG_SF_FEATDMF:
	case SIG_SF_FEATB:
	case SIG_FXOLS:
	case SIG_FXOGS:
	case SIG_FXOKS:
		/* Pick up the line */
		ast_debug(1, "Took %s off hook\n", ast->name);
		if (p->hanguponpolarityswitch) {
			p->polaritydelaytv = ast_tvnow();
		}
		res = dahdi_set_hook(p->subs[SUB_REAL].dfd, DAHDI_OFFHOOK);
		tone_zone_play_tone(p->subs[index].dfd, -1);
		p->dialing = 0;
		if ((index == SUB_REAL) && p->subs[SUB_THREEWAY].inthreeway) {
			if (oldstate == AST_STATE_RINGING) {
				ast_debug(1, "Finally swapping real and threeway\n");
				tone_zone_play_tone(p->subs[SUB_THREEWAY].dfd, -1);
				swap_subs(p, SUB_THREEWAY, SUB_REAL);
				p->owner = p->subs[SUB_REAL].owner;
			}
		}
		if (p->sig & __DAHDI_SIG_FXS) {
			dahdi_enable_ec(p);
			dahdi_train_ec(p);
		}
		break;
#ifdef HAVE_PRI
	case SIG_BRI:
	case SIG_BRI_PTMP:
	case SIG_PRI:
		/* Send a pri acknowledge */
		if (!pri_grab(p, p->pri)) {
			p->proceeding = 1;
			p->dialing = 0;
			res = pri_answer(p->pri->pri, p->call, 0, !p->digital);
			pri_rel(p->pri);
		} else {
			ast_log(LOG_WARNING, "Unable to grab PRI on span %d\n", p->span);
			res = -1;
		}
		break;
#endif
#ifdef HAVE_SS7
	case SIG_SS7:
		if (!ss7_grab(p, p->ss7)) {
			const char *connected_num, *connected_pres_str;
			unsigned char connected_pres;
			char connected_nai;
			int connected_strip = 0;

			connected_num = pbx_builtin_getvar_helper(ast, "SS7_CONNECTED_NUMBER");
			if (connected_num) {
				if (p->ss7->called_nai ==  SS7_NAI_DYNAMIC)
					connected_strip = ss7_parse_prefix(p, connected_num, &connected_nai);
				else
					connected_nai = p->ss7->called_nai;

				connected_pres_str = pbx_builtin_getvar_helper(ast, "SS7_CONNECTED_PRES");

				if (connected_pres_str)
					connected_pres = cid_pres2ss7pres(ast_parse_caller_presentation(connected_pres_str));
				else
					connected_pres = SS7_PRESENTATION_RESTRICTED;

				isup_set_connected(p->ss7call, connected_num + connected_strip, connected_nai, connected_pres, SS7_SCREENING_NETWORK_PROVIDED);
			}
			p->proceeding = 1;
			p->dialing = 0;
			res = isup_anm(p->ss7->ss7, p->ss7call);
			ss7_rel(p->ss7);
		} else {
			ast_log(LOG_WARNING, "Unable to grab SS7 on span %d\n", p->span);
			res = -1;
		}
		break;
#endif
	case 0:
		ast_mutex_unlock(&p->lock);
		return 0;
	default:
		ast_log(LOG_WARNING, "Don't know how to answer signalling %d (channel %d)\n", p->sig, p->channel);
		res = -1;
	}
	ast_mutex_unlock(&p->lock);
	return res;
}

static int dahdi_setoption(struct ast_channel *chan, int option, void *data, int datalen)
{
	char *cp;
	signed char *scp;
	int x;
	int index;
	struct dahdi_pvt *p = chan->tech_pvt, *pp;
	struct oprmode *oprmode;


	/* all supported options require data */
	if (!data || (datalen < 1)) {
		errno = EINVAL;
		return -1;
	}

	switch (option) {
	case AST_OPTION_TXGAIN:
		scp = (signed char *) data;
		index = dahdi_get_index(chan, p, 0);
		if (index < 0) {
			ast_log(LOG_WARNING, "No index in TXGAIN?\n");
			return -1;
		}
		ast_debug(1, "Setting actual tx gain on %s to %f\n", chan->name, p->txgain + (float) *scp);
		return set_actual_txgain(p->subs[index].dfd, 0, p->txgain + (float) *scp, p->law);
	case AST_OPTION_RXGAIN:
		scp = (signed char *) data;
		index = dahdi_get_index(chan, p, 0);
		if (index < 0) {
			ast_log(LOG_WARNING, "No index in RXGAIN?\n");
			return -1;
		}
		ast_debug(1, "Setting actual rx gain on %s to %f\n", chan->name, p->rxgain + (float) *scp);
		return set_actual_rxgain(p->subs[index].dfd, 0, p->rxgain + (float) *scp, p->law);
	case AST_OPTION_TONE_VERIFY:
		if (!p->dsp)
			break;
		cp = (char *) data;
		switch (*cp) {
		case 1:
			ast_debug(1, "Set option TONE VERIFY, mode: MUTECONF(1) on %s\n",chan->name);
			ast_dsp_digitmode(p->dsp, DSP_DIGITMODE_MUTECONF | p->dtmfrelax);  /* set mute mode if desired */
			break;
		case 2:
			ast_debug(1, "Set option TONE VERIFY, mode: MUTECONF/MAX(2) on %s\n",chan->name);
			ast_dsp_digitmode(p->dsp, DSP_DIGITMODE_MUTECONF | DSP_DIGITMODE_MUTEMAX | p->dtmfrelax);  /* set mute mode if desired */
			break;
		default:
			ast_debug(1, "Set option TONE VERIFY, mode: OFF(0) on %s\n",chan->name);
			ast_dsp_digitmode(p->dsp, DSP_DIGITMODE_DTMF | p->dtmfrelax);  /* set mute mode if desired */
			break;
		}
		break;
	case AST_OPTION_TDD:
		/* turn on or off TDD */
		cp = (char *) data;
		p->mate = 0;
		if (!*cp) { /* turn it off */
			ast_debug(1, "Set option TDD MODE, value: OFF(0) on %s\n",chan->name);
			if (p->tdd)
				tdd_free(p->tdd);
			p->tdd = 0;
			break;
		}
		ast_debug(1, "Set option TDD MODE, value: %s(%d) on %s\n",
			(*cp == 2) ? "MATE" : "ON", (int) *cp, chan->name);
		dahdi_disable_ec(p);
		/* otherwise, turn it on */
		if (!p->didtdd) { /* if havent done it yet */
			unsigned char mybuf[41000], *buf;
			int size, res, fd, len;
			struct pollfd fds[1];

			buf = mybuf;
			memset(buf, 0x7f, sizeof(mybuf)); /* set to silence */
			ast_tdd_gen_ecdisa(buf + 16000, 16000);  /* put in tone */
			len = 40000;
			index = dahdi_get_index(chan, p, 0);
			if (index < 0) {
				ast_log(LOG_WARNING, "No index in TDD?\n");
				return -1;
			}
			fd = p->subs[index].dfd;
			while (len) {
				if (ast_check_hangup(chan))
					return -1;
				size = len;
				if (size > READ_SIZE)
					size = READ_SIZE;
				fds[0].fd = fd;
				fds[0].events = POLLPRI | POLLOUT;
				fds[0].revents = 0;
				res = poll(fds, 1, -1);
				if (!res) {
					ast_debug(1, "poll (for write) ret. 0 on channel %d\n", p->channel);
					continue;
				}
				/* if got exception */
				if (fds[0].revents & POLLPRI)
					return -1;
				if (!(fds[0].revents & POLLOUT)) {
					ast_debug(1, "write fd not ready on channel %d\n", p->channel);
					continue;
				}
				res = write(fd, buf, size);
				if (res != size) {
					if (res == -1) return -1;
					ast_debug(1, "Write returned %d (%s) on channel %d\n", res, strerror(errno), p->channel);
					break;
				}
				len -= size;
				buf += size;
			}
			p->didtdd = 1; /* set to have done it now */
		}
		if (*cp == 2) { /* Mate mode */
			if (p->tdd)
				tdd_free(p->tdd);
			p->tdd = 0;
			p->mate = 1;
			break;
		}
		if (!p->tdd) { /* if we dont have one yet */
			p->tdd = tdd_new(); /* allocate one */
		}
		break;
	case AST_OPTION_RELAXDTMF:  /* Relax DTMF decoding (or not) */
		if (!p->dsp)
			break;
		cp = (char *) data;
		ast_debug(1, "Set option RELAX DTMF, value: %s(%d) on %s\n",
			*cp ? "ON" : "OFF", (int) *cp, chan->name);
		ast_dsp_digitmode(p->dsp, ((*cp) ? DSP_DIGITMODE_RELAXDTMF : DSP_DIGITMODE_DTMF) | p->dtmfrelax);
		break;
	case AST_OPTION_AUDIO_MODE:  /* Set AUDIO mode (or not) */
		cp = (char *) data;
		if (!*cp) {
			ast_debug(1, "Set option AUDIO MODE, value: OFF(0) on %s\n", chan->name);
			x = 0;
			dahdi_disable_ec(p);
		} else {
			ast_debug(1, "Set option AUDIO MODE, value: ON(1) on %s\n", chan->name);
			x = 1;
		}
		if (ioctl(p->subs[SUB_REAL].dfd, DAHDI_AUDIOMODE, &x) == -1)
			ast_log(LOG_WARNING, "Unable to set audio mode on channel %d to %d: %s\n", p->channel, x, strerror(errno));
		break;
	case AST_OPTION_OPRMODE:  /* Operator services mode */
		oprmode = (struct oprmode *) data;
		pp = oprmode->peer->tech_pvt;
		p->oprmode = pp->oprmode = 0;
		/* setup peers */
		p->oprpeer = pp;
		pp->oprpeer = p;
		/* setup modes, if any */
		if (oprmode->mode)
		{
			pp->oprmode = oprmode->mode;
			p->oprmode = -oprmode->mode;
		}
		ast_debug(1, "Set Operator Services mode, value: %d on %s/%s\n",
			oprmode->mode, chan->name,oprmode->peer->name);
		break;
	case AST_OPTION_ECHOCAN:
		cp = (char *) data;
		if (*cp) {
			ast_debug(1, "Enabling echo cancelation on %s\n", chan->name);
			dahdi_enable_ec(p);
		} else {
			ast_debug(1, "Disabling echo cancelation on %s\n", chan->name);
			dahdi_disable_ec(p);
		}
		break;
	}
	errno = 0;

	return 0;
}

static int dahdi_func_read(struct ast_channel *chan, const char *function, char *data, char *buf, size_t len)
{
	struct dahdi_pvt *p = chan->tech_pvt;

	if (!strcasecmp(data, "rxgain")) {
		ast_mutex_lock(&p->lock);
		snprintf(buf, len, "%f", p->rxgain);
		ast_mutex_unlock(&p->lock);
	} else if (!strcasecmp(data, "txgain")) {
		ast_mutex_lock(&p->lock);
		snprintf(buf, len, "%f", p->txgain);
		ast_mutex_unlock(&p->lock);
	} else {
		ast_copy_string(buf, "", len);
	}
	return 0;
}


static void dahdi_unlink(struct dahdi_pvt *slave, struct dahdi_pvt *master, int needlock)
{
	/* Unlink a specific slave or all slaves/masters from a given master */
	int x;
	int hasslaves;
	if (!master)
		return;
	if (needlock) {
		ast_mutex_lock(&master->lock);
		if (slave) {
			while (ast_mutex_trylock(&slave->lock)) {
				DEADLOCK_AVOIDANCE(&master->lock);
			}
		}
	}
	hasslaves = 0;
	for (x = 0; x < MAX_SLAVES; x++) {
		if (master->slaves[x]) {
			if (!slave || (master->slaves[x] == slave)) {
				/* Take slave out of the conference */
				ast_debug(1, "Unlinking slave %d from %d\n", master->slaves[x]->channel, master->channel);
				conf_del(master, &master->slaves[x]->subs[SUB_REAL], SUB_REAL);
				conf_del(master->slaves[x], &master->subs[SUB_REAL], SUB_REAL);
				master->slaves[x]->master = NULL;
				master->slaves[x] = NULL;
			} else
				hasslaves = 1;
		}
		if (!hasslaves)
			master->inconference = 0;
	}
	if (!slave) {
		if (master->master) {
			/* Take master out of the conference */
			conf_del(master->master, &master->subs[SUB_REAL], SUB_REAL);
			conf_del(master, &master->master->subs[SUB_REAL], SUB_REAL);
			hasslaves = 0;
			for (x = 0; x < MAX_SLAVES; x++) {
				if (master->master->slaves[x] == master)
					master->master->slaves[x] = NULL;
				else if (master->master->slaves[x])
					hasslaves = 1;
			}
			if (!hasslaves)
				master->master->inconference = 0;
		}
		master->master = NULL;
	}
	update_conf(master);
	if (needlock) {
		if (slave)
			ast_mutex_unlock(&slave->lock);
		ast_mutex_unlock(&master->lock);
	}
}

static void dahdi_link(struct dahdi_pvt *slave, struct dahdi_pvt *master) {
	int x;
	if (!slave || !master) {
		ast_log(LOG_WARNING, "Tried to link to/from NULL??\n");
		return;
	}
	for (x = 0; x < MAX_SLAVES; x++) {
		if (!master->slaves[x]) {
			master->slaves[x] = slave;
			break;
		}
	}
	if (x >= MAX_SLAVES) {
		ast_log(LOG_WARNING, "Replacing slave %d with new slave, %d\n", master->slaves[MAX_SLAVES - 1]->channel, slave->channel);
		master->slaves[MAX_SLAVES - 1] = slave;
	}
	if (slave->master)
		ast_log(LOG_WARNING, "Replacing master %d with new master, %d\n", slave->master->channel, master->channel);
	slave->master = master;

	ast_debug(1, "Making %d slave to master %d at %d\n", slave->channel, master->channel, x);
}

static void disable_dtmf_detect(struct dahdi_pvt *p)
{
	int val;

	p->ignoredtmf = 1;

	val = 0;
	ioctl(p->subs[SUB_REAL].dfd, DAHDI_TONEDETECT, &val);

	if (!p->hardwaredtmf && p->dsp) {
		p->dsp_features &= ~DSP_FEATURE_DTMF_DETECT;
		ast_dsp_set_features(p->dsp, p->dsp_features);
	}
}

static void enable_dtmf_detect(struct dahdi_pvt *p)
{
	int val;

	if (p->channel == CHAN_PSEUDO)
		return;

	p->ignoredtmf = 0;

	val = DAHDI_TONEDETECT_ON | DAHDI_TONEDETECT_MUTE;
	ioctl(p->subs[SUB_REAL].dfd, DAHDI_TONEDETECT, &val);

	if (!p->hardwaredtmf && p->dsp) {
		p->dsp_features |= DSP_FEATURE_DTMF_DETECT;
		ast_dsp_set_features(p->dsp, p->dsp_features);
	}
}

static enum ast_bridge_result dahdi_bridge(struct ast_channel *c0, struct ast_channel *c1, int flags, struct ast_frame **fo, struct ast_channel **rc, int timeoutms)
{
	struct ast_channel *who;
	struct dahdi_pvt *p0, *p1, *op0, *op1;
	struct dahdi_pvt *master = NULL, *slave = NULL;
	struct ast_frame *f;
	int inconf = 0;
	int nothingok = 1;
	int ofd0, ofd1;
	int oi0, oi1, i0 = -1, i1 = -1, t0, t1;
	int os0 = -1, os1 = -1;
	int priority = 0;
	struct ast_channel *oc0, *oc1;
	enum ast_bridge_result res;

#ifdef PRI_2BCT
	int triedtopribridge = 0;
	q931_call *q931c0 = NULL, *q931c1 = NULL;
#endif

	/* For now, don't attempt to native bridge if either channel needs DTMF detection.
	   There is code below to handle it properly until DTMF is actually seen,
	   but due to currently unresolved issues it's ignored...
	*/

	if (flags & (AST_BRIDGE_DTMF_CHANNEL_0 | AST_BRIDGE_DTMF_CHANNEL_1))
		return AST_BRIDGE_FAILED_NOWARN;

	ast_channel_lock(c0);
	while (ast_channel_trylock(c1)) {
		ast_channel_unlock(c0);
		usleep(1);
		ast_channel_lock(c0);
	}

	p0 = c0->tech_pvt;
	p1 = c1->tech_pvt;
	/* cant do pseudo-channels here */
	if (!p0 || (!p0->sig) || !p1 || (!p1->sig)) {
		ast_channel_unlock(c0);
		ast_channel_unlock(c1);
		return AST_BRIDGE_FAILED_NOWARN;
	}

	oi0 = dahdi_get_index(c0, p0, 0);
	oi1 = dahdi_get_index(c1, p1, 0);
	if ((oi0 < 0) || (oi1 < 0)) {
		ast_channel_unlock(c0);
		ast_channel_unlock(c1);
		return AST_BRIDGE_FAILED;
	}

	op0 = p0 = c0->tech_pvt;
	op1 = p1 = c1->tech_pvt;
	ofd0 = c0->fds[0];
	ofd1 = c1->fds[0];
	oc0 = p0->owner;
	oc1 = p1->owner;

	if (ast_mutex_trylock(&p0->lock)) {
		/* Don't block, due to potential for deadlock */
		ast_channel_unlock(c0);
		ast_channel_unlock(c1);
		ast_log(LOG_NOTICE, "Avoiding deadlock...\n");
		return AST_BRIDGE_RETRY;
	}
	if (ast_mutex_trylock(&p1->lock)) {
		/* Don't block, due to potential for deadlock */
		ast_mutex_unlock(&p0->lock);
		ast_channel_unlock(c0);
		ast_channel_unlock(c1);
		ast_log(LOG_NOTICE, "Avoiding deadlock...\n");
		return AST_BRIDGE_RETRY;
	}

	if ((oi0 == SUB_REAL) && (oi1 == SUB_REAL)) {
		if (p0->owner && p1->owner) {
			/* If we don't have a call-wait in a 3-way, and we aren't in a 3-way, we can be master */
			if (!p0->subs[SUB_CALLWAIT].inthreeway && !p1->subs[SUB_REAL].inthreeway) {
				master = p0;
				slave = p1;
				inconf = 1;
			} else if (!p1->subs[SUB_CALLWAIT].inthreeway && !p0->subs[SUB_REAL].inthreeway) {
				master = p1;
				slave = p0;
				inconf = 1;
			} else {
				ast_log(LOG_WARNING, "Huh?  Both calls are callwaits or 3-ways?  That's clever...?\n");
				ast_log(LOG_WARNING, "p0: chan %d/%d/CW%d/3W%d, p1: chan %d/%d/CW%d/3W%d\n",
					p0->channel,
					oi0, (p0->subs[SUB_CALLWAIT].dfd > -1) ? 1 : 0,
					p0->subs[SUB_REAL].inthreeway, p0->channel,
					oi0, (p1->subs[SUB_CALLWAIT].dfd > -1) ? 1 : 0,
					p1->subs[SUB_REAL].inthreeway);
			}
			nothingok = 0;
		}
	} else if ((oi0 == SUB_REAL) && (oi1 == SUB_THREEWAY)) {
		if (p1->subs[SUB_THREEWAY].inthreeway) {
			master = p1;
			slave = p0;
			nothingok = 0;
		}
	} else if ((oi0 == SUB_THREEWAY) && (oi1 == SUB_REAL)) {
		if (p0->subs[SUB_THREEWAY].inthreeway) {
			master = p0;
			slave = p1;
			nothingok = 0;
		}
	} else if ((oi0 == SUB_REAL) && (oi1 == SUB_CALLWAIT)) {
		/* We have a real and a call wait.  If we're in a three way call, put us in it, otherwise,
		   don't put us in anything */
		if (p1->subs[SUB_CALLWAIT].inthreeway) {
			master = p1;
			slave = p0;
			nothingok = 0;
		}
	} else if ((oi0 == SUB_CALLWAIT) && (oi1 == SUB_REAL)) {
		/* Same as previous */
		if (p0->subs[SUB_CALLWAIT].inthreeway) {
			master = p0;
			slave = p1;
			nothingok = 0;
		}
	}
	ast_debug(1, "master: %d, slave: %d, nothingok: %d\n",
		master ? master->channel : 0, slave ? slave->channel : 0, nothingok);
	if (master && slave) {
		/* Stop any tones, or play ringtone as appropriate.  If they're bridged
		   in an active threeway call with a channel that is ringing, we should
		   indicate ringing. */
		if ((oi1 == SUB_THREEWAY) &&
		    p1->subs[SUB_THREEWAY].inthreeway &&
		    p1->subs[SUB_REAL].owner &&
		    p1->subs[SUB_REAL].inthreeway &&
		    (p1->subs[SUB_REAL].owner->_state == AST_STATE_RINGING)) {
			ast_debug(1, "Playing ringback on %s since %s is in a ringing three-way\n", c0->name, c1->name);
			tone_zone_play_tone(p0->subs[oi0].dfd, DAHDI_TONE_RINGTONE);
			os1 = p1->subs[SUB_REAL].owner->_state;
		} else {
			ast_debug(1, "Stopping tones on %d/%d talking to %d/%d\n", p0->channel, oi0, p1->channel, oi1);
			tone_zone_play_tone(p0->subs[oi0].dfd, -1);
		}
		if ((oi0 == SUB_THREEWAY) &&
		    p0->subs[SUB_THREEWAY].inthreeway &&
		    p0->subs[SUB_REAL].owner &&
		    p0->subs[SUB_REAL].inthreeway &&
		    (p0->subs[SUB_REAL].owner->_state == AST_STATE_RINGING)) {
			ast_debug(1, "Playing ringback on %s since %s is in a ringing three-way\n", c1->name, c0->name);
			tone_zone_play_tone(p1->subs[oi1].dfd, DAHDI_TONE_RINGTONE);
			os0 = p0->subs[SUB_REAL].owner->_state;
		} else {
			ast_debug(1, "Stopping tones on %d/%d talking to %d/%d\n", p1->channel, oi1, p0->channel, oi0);
			tone_zone_play_tone(p1->subs[oi0].dfd, -1);
		}
		if ((oi0 == SUB_REAL) && (oi1 == SUB_REAL)) {
			if (!p0->echocanbridged || !p1->echocanbridged) {
				/* Disable echo cancellation if appropriate */
				dahdi_disable_ec(p0);
				dahdi_disable_ec(p1);
			}
		}
		dahdi_link(slave, master);
		master->inconference = inconf;
	} else if (!nothingok)
		ast_log(LOG_WARNING, "Can't link %d/%s with %d/%s\n", p0->channel, subnames[oi0], p1->channel, subnames[oi1]);

	update_conf(p0);
	update_conf(p1);
	t0 = p0->subs[SUB_REAL].inthreeway;
	t1 = p1->subs[SUB_REAL].inthreeway;

	ast_mutex_unlock(&p0->lock);
	ast_mutex_unlock(&p1->lock);

	ast_channel_unlock(c0);
	ast_channel_unlock(c1);

	/* Native bridge failed */
	if ((!master || !slave) && !nothingok) {
		dahdi_enable_ec(p0);
		dahdi_enable_ec(p1);
		return AST_BRIDGE_FAILED;
	}

	ast_verb(3, "Native bridging %s and %s\n", c0->name, c1->name);

	if (!(flags & AST_BRIDGE_DTMF_CHANNEL_0) && (oi0 == SUB_REAL))
		disable_dtmf_detect(op0);

	if (!(flags & AST_BRIDGE_DTMF_CHANNEL_1) && (oi1 == SUB_REAL))
		disable_dtmf_detect(op1);

	for (;;) {
		struct ast_channel *c0_priority[2] = {c0, c1};
		struct ast_channel *c1_priority[2] = {c1, c0};

		/* Here's our main loop...  Start by locking things, looking for private parts,
		   and then balking if anything is wrong */

		ast_channel_lock(c0);
		while (ast_channel_trylock(c1)) {
			ast_channel_unlock(c0);
			usleep(1);
			ast_channel_lock(c0);
		}

		p0 = c0->tech_pvt;
		p1 = c1->tech_pvt;

		if (op0 == p0)
			i0 = dahdi_get_index(c0, p0, 1);
		if (op1 == p1)
			i1 = dahdi_get_index(c1, p1, 1);

		ast_channel_unlock(c0);
		ast_channel_unlock(c1);

		if (!timeoutms ||
		    (op0 != p0) ||
		    (op1 != p1) ||
		    (ofd0 != c0->fds[0]) ||
		    (ofd1 != c1->fds[0]) ||
		    (p0->subs[SUB_REAL].owner && (os0 > -1) && (os0 != p0->subs[SUB_REAL].owner->_state)) ||
		    (p1->subs[SUB_REAL].owner && (os1 > -1) && (os1 != p1->subs[SUB_REAL].owner->_state)) ||
		    (oc0 != p0->owner) ||
		    (oc1 != p1->owner) ||
		    (t0 != p0->subs[SUB_REAL].inthreeway) ||
		    (t1 != p1->subs[SUB_REAL].inthreeway) ||
		    (oi0 != i0) ||
		    (oi1 != i1)) {
			ast_debug(1, "Something changed out on %d/%d to %d/%d, returning -3 to restart\n",
				op0->channel, oi0, op1->channel, oi1);
			res = AST_BRIDGE_RETRY;
			goto return_from_bridge;
		}

#ifdef PRI_2BCT
		q931c0 = p0->call;
		q931c1 = p1->call;
		if (p0->transfer && p1->transfer
		    && q931c0 && q931c1
		    && !triedtopribridge) {
			pri_channel_bridge(q931c0, q931c1);
			triedtopribridge = 1;
		}
#endif

		who = ast_waitfor_n(priority ? c0_priority : c1_priority, 2, &timeoutms);
		if (!who) {
			ast_debug(1, "Ooh, empty read...\n");
			continue;
		}
		f = ast_read(who);
		if (!f || (f->frametype == AST_FRAME_CONTROL)) {
			*fo = f;
			*rc = who;
			res = AST_BRIDGE_COMPLETE;
			goto return_from_bridge;
		}
		if (f->frametype == AST_FRAME_DTMF) {
			if ((who == c0) && p0->pulsedial) {
				ast_write(c1, f);
			} else if ((who == c1) && p1->pulsedial) {
				ast_write(c0, f);
			} else {
				*fo = f;
				*rc = who;
				res = AST_BRIDGE_COMPLETE;
				goto return_from_bridge;
			}
		}
		ast_frfree(f);

		/* Swap who gets priority */
		priority = !priority;
	}

return_from_bridge:
	if (op0 == p0)
		dahdi_enable_ec(p0);

	if (op1 == p1)
		dahdi_enable_ec(p1);

	if (!(flags & AST_BRIDGE_DTMF_CHANNEL_0) && (oi0 == SUB_REAL))
		enable_dtmf_detect(op0);

	if (!(flags & AST_BRIDGE_DTMF_CHANNEL_1) && (oi1 == SUB_REAL))
		enable_dtmf_detect(op1);

	dahdi_unlink(slave, master, 1);

	return res;
}

static int dahdi_fixup(struct ast_channel *oldchan, struct ast_channel *newchan)
{
	struct dahdi_pvt *p = newchan->tech_pvt;
	int x;
	ast_mutex_lock(&p->lock);
	ast_debug(1, "New owner for channel %d is %s\n", p->channel, newchan->name);
	if (p->owner == oldchan) {
		p->owner = newchan;
	}
	for (x = 0; x < 3; x++)
		if (p->subs[x].owner == oldchan) {
			if (!x)
				dahdi_unlink(NULL, p, 0);
			p->subs[x].owner = newchan;
		}
	if (newchan->_state == AST_STATE_RINGING)
		dahdi_indicate(newchan, AST_CONTROL_RINGING, NULL, 0);
	update_conf(p);
	ast_mutex_unlock(&p->lock);
	return 0;
}

static int dahdi_ring_phone(struct dahdi_pvt *p)
{
	int x;
	int res;
	/* Make sure our transmit state is on hook */
	x = 0;
	x = DAHDI_ONHOOK;
	res = ioctl(p->subs[SUB_REAL].dfd, DAHDI_HOOK, &x);
	do {
		x = DAHDI_RING;
		res = ioctl(p->subs[SUB_REAL].dfd, DAHDI_HOOK, &x);
		if (res) {
			switch (errno) {
			case EBUSY:
			case EINTR:
				/* Wait just in case */
				usleep(10000);
				continue;
			case EINPROGRESS:
				res = 0;
				break;
			default:
				ast_log(LOG_WARNING, "Couldn't ring the phone: %s\n", strerror(errno));
				res = 0;
			}
		}
	} while (res);
	return res;
}

static void *ss_thread(void *data);

static struct ast_channel *dahdi_new(struct dahdi_pvt *, int, int, int, int, int);

static int attempt_transfer(struct dahdi_pvt *p)
{
	/* In order to transfer, we need at least one of the channels to
	   actually be in a call bridge.  We can't conference two applications
	   together (but then, why would we want to?) */
	if (ast_bridged_channel(p->subs[SUB_REAL].owner)) {
		/* The three-way person we're about to transfer to could still be in MOH, so
		   stop if now if appropriate */
		if (ast_bridged_channel(p->subs[SUB_THREEWAY].owner))
			ast_queue_control(p->subs[SUB_THREEWAY].owner, AST_CONTROL_UNHOLD);
		if (p->subs[SUB_REAL].owner->_state == AST_STATE_RINGING) {
			ast_indicate(ast_bridged_channel(p->subs[SUB_REAL].owner), AST_CONTROL_RINGING);
		}
		if (p->subs[SUB_THREEWAY].owner->_state == AST_STATE_RING) {
			tone_zone_play_tone(p->subs[SUB_THREEWAY].dfd, DAHDI_TONE_RINGTONE);
		}
		 if (ast_channel_masquerade(p->subs[SUB_THREEWAY].owner, ast_bridged_channel(p->subs[SUB_REAL].owner))) {
			ast_log(LOG_WARNING, "Unable to masquerade %s as %s\n",
					ast_bridged_channel(p->subs[SUB_REAL].owner)->name, p->subs[SUB_THREEWAY].owner->name);
			return -1;
		}
		/* Orphan the channel after releasing the lock */
		ast_channel_unlock(p->subs[SUB_THREEWAY].owner);
		unalloc_sub(p, SUB_THREEWAY);
	} else if (ast_bridged_channel(p->subs[SUB_THREEWAY].owner)) {
		ast_queue_control(p->subs[SUB_REAL].owner, AST_CONTROL_UNHOLD);
		if (p->subs[SUB_THREEWAY].owner->_state == AST_STATE_RINGING) {
			ast_indicate(ast_bridged_channel(p->subs[SUB_THREEWAY].owner), AST_CONTROL_RINGING);
		}
		if (p->subs[SUB_REAL].owner->_state == AST_STATE_RING) {
			tone_zone_play_tone(p->subs[SUB_REAL].dfd, DAHDI_TONE_RINGTONE);
		}
		if (ast_channel_masquerade(p->subs[SUB_REAL].owner, ast_bridged_channel(p->subs[SUB_THREEWAY].owner))) {
			ast_log(LOG_WARNING, "Unable to masquerade %s as %s\n",
					ast_bridged_channel(p->subs[SUB_THREEWAY].owner)->name, p->subs[SUB_REAL].owner->name);
			return -1;
		}
		/* Three-way is now the REAL */
		swap_subs(p, SUB_THREEWAY, SUB_REAL);
		ast_channel_unlock(p->subs[SUB_REAL].owner);
		unalloc_sub(p, SUB_THREEWAY);
		/* Tell the caller not to hangup */
		return 1;
	} else {
		ast_debug(1, "Neither %s nor %s are in a bridge, nothing to transfer\n",
			p->subs[SUB_REAL].owner->name, p->subs[SUB_THREEWAY].owner->name);
		p->subs[SUB_THREEWAY].owner->_softhangup |= AST_SOFTHANGUP_DEV;
		return -1;
	}
	return 0;
}

static int check_for_conference(struct dahdi_pvt *p)
{
	struct dahdi_confinfo ci;
	/* Fine if we already have a master, etc */
	if (p->master || (p->confno > -1))
		return 0;
	memset(&ci, 0, sizeof(ci));
	if (ioctl(p->subs[SUB_REAL].dfd, DAHDI_GETCONF, &ci)) {
		ast_log(LOG_WARNING, "Failed to get conference info on channel %d: %s\n", p->channel, strerror(errno));
		return 0;
	}
	/* If we have no master and don't have a confno, then
	   if we're in a conference, it's probably a MeetMe room or
	   some such, so don't let us 3-way out! */
	if ((p->subs[SUB_REAL].curconf.confno != ci.confno) || (p->subs[SUB_REAL].curconf.confmode != ci.confmode)) {
		ast_verb(3, "Avoiding 3-way call when in an external conference\n");
		return 1;
	}
	return 0;
}

/*! Checks channel for alarms
 * \param p a channel to check for alarms.
 * \returns the alarms on the span to which the channel belongs, or alarms on
 *          the channel if no span alarms.
 */
static int get_alarms(struct dahdi_pvt *p)
{
	int res;
	struct dahdi_spaninfo zi;
	struct dahdi_params params;

	memset(&zi, 0, sizeof(zi));
	zi.spanno = p->span;

	if ((res = ioctl(p->subs[SUB_REAL].dfd, DAHDI_SPANSTAT, &zi)) >= 0) {
		if (zi.alarms != DAHDI_ALARM_NONE)
			return zi.alarms;
	} else {
		ast_log(LOG_WARNING, "Unable to determine alarm on channel %d: %s\n", p->channel, strerror(errno));
		return 0;
	}

	/* No alarms on the span. Check for channel alarms. */
	memset(&params, 0, sizeof(params));
	if ((res = ioctl(p->subs[SUB_REAL].dfd, DAHDI_GET_PARAMS, &params)) >= 0)
		return params.chan_alarms;

	ast_log(LOG_WARNING, "Unable to determine alarm on channel %d\n", p->channel);

	return DAHDI_ALARM_NONE;
}

static void dahdi_handle_dtmfup(struct ast_channel *ast, int index, struct ast_frame **dest)
{
	struct dahdi_pvt *p = ast->tech_pvt;
	struct ast_frame *f = *dest;

	ast_debug(1, "DTMF digit: %c on %s\n", f->subclass, ast->name);

	if (p->confirmanswer) {
		ast_debug(1, "Confirm answer on %s!\n", ast->name);
		/* Upon receiving a DTMF digit, consider this an answer confirmation instead
		   of a DTMF digit */
		p->subs[index].f.frametype = AST_FRAME_CONTROL;
		p->subs[index].f.subclass = AST_CONTROL_ANSWER;
		*dest = &p->subs[index].f;
		/* Reset confirmanswer so DTMF's will behave properly for the duration of the call */
		p->confirmanswer = 0;
	} else if (p->callwaitcas) {
		if ((f->subclass == 'A') || (f->subclass == 'D')) {
			ast_debug(1, "Got some DTMF, but it's for the CAS\n");
			if (p->cidspill)
				ast_free(p->cidspill);
			send_cwcidspill(p);
		}
		if ((f->subclass != 'm') && (f->subclass != 'u'))
			p->callwaitcas = 0;
		p->subs[index].f.frametype = AST_FRAME_NULL;
		p->subs[index].f.subclass = 0;
		*dest = &p->subs[index].f;
	} else if (f->subclass == 'f') {
		/* Fax tone -- Handle and return NULL */
		if ((p->callprogress & CALLPROGRESS_FAX) && !p->faxhandled) {
			p->faxhandled++;
			if (strcmp(ast->exten, "fax")) {
				const char *target_context = S_OR(ast->macrocontext, ast->context);

				/* We need to unlock 'ast' here because ast_exists_extension has the
				 * potential to start autoservice on the channel. Such action is prone
				 * to deadlock.
				 */
				ast_mutex_unlock(&p->lock);
				ast_channel_unlock(ast);
				if (ast_exists_extension(ast, target_context, "fax", 1, ast->cid.cid_num)) {
					ast_channel_lock(ast);
					ast_mutex_lock(&p->lock);
					ast_verb(3, "Redirecting %s to fax extension\n", ast->name);
					/* Save the DID/DNIS when we transfer the fax call to a "fax" extension */
					pbx_builtin_setvar_helper(ast, "FAXEXTEN", ast->exten);
					if (ast_async_goto(ast, target_context, "fax", 1))
						ast_log(LOG_WARNING, "Failed to async goto '%s' into fax of '%s'\n", ast->name, target_context);
				} else {
					ast_channel_lock(ast);
					ast_mutex_lock(&p->lock);
					ast_log(LOG_NOTICE, "Fax detected, but no fax extension\n");
				}
			} else {
				ast_debug(1, "Already in a fax extension, not redirecting\n");
			}
		} else {
			ast_debug(1, "Fax already handled\n");
		}
		dahdi_confmute(p, 0);
		p->subs[index].f.frametype = AST_FRAME_NULL;
		p->subs[index].f.subclass = 0;
		*dest = &p->subs[index].f;
	} else if (f->subclass == 'm') {
		/* Confmute request */
		dahdi_confmute(p, 1);
		p->subs[index].f.frametype = AST_FRAME_NULL;
		p->subs[index].f.subclass = 0;
		*dest = &p->subs[index].f;
	} else if (f->subclass == 'u') {
		/* Unmute */
		dahdi_confmute(p, 0);
		p->subs[index].f.frametype = AST_FRAME_NULL;
		p->subs[index].f.subclass = 0;
		*dest = &p->subs[index].f;
	} else
		dahdi_confmute(p, 0);
}

static void handle_alarms(struct dahdi_pvt *p, int alarms)
{
	const char *alarm_str = alarm2str(alarms);

	ast_log(LOG_WARNING, "Detected alarm on channel %d: %s\n", p->channel, alarm_str);
	manager_event(EVENT_FLAG_SYSTEM, "Alarm",
		      "Alarm: %s\r\n"
		      "Channel: %d\r\n",
		      alarm_str, p->channel);
}

static struct ast_frame *dahdi_handle_event(struct ast_channel *ast)
{
	int res, x;
	int index, mysig;
	char *c;
	struct dahdi_pvt *p = ast->tech_pvt;
	pthread_t threadid;
	struct ast_channel *chan;
	struct ast_frame *f;

	index = dahdi_get_index(ast, p, 0);
	mysig = p->sig;
	if (p->outsigmod > -1)
		mysig = p->outsigmod;
	p->subs[index].f.frametype = AST_FRAME_NULL;
	p->subs[index].f.subclass = 0;
	p->subs[index].f.datalen = 0;
	p->subs[index].f.samples = 0;
	p->subs[index].f.mallocd = 0;
	p->subs[index].f.offset = 0;
	p->subs[index].f.src = "dahdi_handle_event";
	p->subs[index].f.data = NULL;
	f = &p->subs[index].f;

	if (index < 0)
		return &p->subs[index].f;
	if (p->fake_event) {
		res = p->fake_event;
		p->fake_event = 0;
	} else
		res = dahdi_get_event(p->subs[index].dfd);

	ast_debug(1, "Got event %s(%d) on channel %d (index %d)\n", event2str(res), res, p->channel, index);

	if (res & (DAHDI_EVENT_PULSEDIGIT | DAHDI_EVENT_DTMFUP)) {
		p->pulsedial =  (res & DAHDI_EVENT_PULSEDIGIT) ? 1 : 0;
		ast_debug(1, "Detected %sdigit '%c'\n", p->pulsedial ? "pulse ": "", res & 0xff);
#ifdef HAVE_PRI
		if (!p->proceeding && ((p->sig == SIG_PRI) || (p->sig == SIG_BRI) || (p->sig == SIG_BRI_PTMP)) && p->pri && (p->pri->overlapdial & DAHDI_OVERLAPDIAL_INCOMING)) {
			/* absorb event */
		} else {
#endif
			p->subs[index].f.frametype = AST_FRAME_DTMF_END;
			p->subs[index].f.subclass = res & 0xff;
#ifdef HAVE_PRI
		}
#endif
		dahdi_handle_dtmfup(ast, index, &f);
		return f;
	}

	if (res & DAHDI_EVENT_DTMFDOWN) {
		ast_debug(1, "DTMF Down '%c'\n", res & 0xff);
		/* Mute conference */
		dahdi_confmute(p, 1);
		p->subs[index].f.frametype = AST_FRAME_DTMF_BEGIN;
		p->subs[index].f.subclass = res & 0xff;
		return &p->subs[index].f;
	}

	switch (res) {
		case DAHDI_EVENT_EC_DISABLED:
			ast_verb(3, "Channel %d echo canceler disabled due to CED detection\n", p->channel);
			p->echocanon = 0;
			break;
		case DAHDI_EVENT_BITSCHANGED:
			ast_log(LOG_WARNING, "Recieved bits changed on %s signalling?\n", sig2str(p->sig));
		case DAHDI_EVENT_PULSE_START:
			/* Stop tone if there's a pulse start and the PBX isn't started */
			if (!ast->pbx)
				tone_zone_play_tone(p->subs[index].dfd, -1);
			break;
		case DAHDI_EVENT_DIALCOMPLETE:
			if (p->inalarm) break;
			if ((p->radio || (p->oprmode < 0))) break;
			if (ioctl(p->subs[index].dfd,DAHDI_DIALING,&x) == -1) {
				ast_log(LOG_DEBUG, "DAHDI_DIALING ioctl failed on %s: %s\n",ast->name, strerror(errno));
				return NULL;
			}
			if (!x) { /* if not still dialing in driver */
				dahdi_enable_ec(p);
				if (p->echobreak) {
					dahdi_train_ec(p);
					ast_copy_string(p->dop.dialstr, p->echorest, sizeof(p->dop.dialstr));
					p->dop.op = DAHDI_DIAL_OP_REPLACE;
					res = ioctl(p->subs[SUB_REAL].dfd, DAHDI_DIAL, &p->dop);
					p->echobreak = 0;
				} else {
					p->dialing = 0;
					if ((mysig == SIG_E911) || (mysig == SIG_FGC_CAMA) || (mysig == SIG_FGC_CAMAMF)) {
						/* if thru with dialing after offhook */
						if (ast->_state == AST_STATE_DIALING_OFFHOOK) {
							ast_setstate(ast, AST_STATE_UP);
							p->subs[index].f.frametype = AST_FRAME_CONTROL;
							p->subs[index].f.subclass = AST_CONTROL_ANSWER;
							break;
						} else { /* if to state wait for offhook to dial rest */
							/* we now wait for off hook */
							ast_setstate(ast,AST_STATE_DIALING_OFFHOOK);
						}
					}
					if (ast->_state == AST_STATE_DIALING) {
						if ((p->callprogress & CALLPROGRESS_PROGRESS) && CANPROGRESSDETECT(p) && p->dsp && p->outgoing) {
							ast_debug(1, "Done dialing, but waiting for progress detection before doing more...\n");
						} else if (p->confirmanswer || (!p->dialednone && ((mysig == SIG_EM) || (mysig == SIG_EM_E1) ||  (mysig == SIG_EMWINK) || (mysig == SIG_FEATD) || (mysig == SIG_FEATDMF_TA) || (mysig == SIG_FEATDMF) || (mysig == SIG_E911) || (mysig == SIG_FGC_CAMA) || (mysig == SIG_FGC_CAMAMF) || (mysig == SIG_FEATB) || (mysig == SIG_SF) || (mysig == SIG_SFWINK) || (mysig == SIG_SF_FEATD) || (mysig == SIG_SF_FEATDMF) || (mysig == SIG_SF_FEATB)))) {
							ast_setstate(ast, AST_STATE_RINGING);
						} else if (!p->answeronpolarityswitch) {
							ast_setstate(ast, AST_STATE_UP);
							p->subs[index].f.frametype = AST_FRAME_CONTROL;
							p->subs[index].f.subclass = AST_CONTROL_ANSWER;
							/* If aops=0 and hops=1, this is necessary */
							p->polarity = POLARITY_REV;
						} else {
							/* Start clean, so we can catch the change to REV polarity when party answers */
							p->polarity = POLARITY_IDLE;
						}
					}
				}
			}
			break;
		case DAHDI_EVENT_ALARM:
#ifdef HAVE_PRI
			if ((p->sig == SIG_PRI) || (p->sig == SIG_BRI) || (p->sig == SIG_BRI_PTMP)) {
				if (!p->pri || !p->pri->pri || (pri_get_timer(p->pri->pri, PRI_TIMER_T309) < 0)) {
					/* T309 is not enabled : hangup calls when alarm occurs */
					if (p->call) {
						if (p->pri && p->pri->pri) {
							if (!pri_grab(p, p->pri)) {
								pri_hangup(p->pri->pri, p->call, -1);
								pri_destroycall(p->pri->pri, p->call);
								p->call = NULL;
								pri_rel(p->pri);
							} else
								ast_log(LOG_WARNING, "Failed to grab PRI!\n");
						} else
							ast_log(LOG_WARNING, "The PRI Call has not been destroyed\n");
					}
					if (p->owner)
						p->owner->_softhangup |= AST_SOFTHANGUP_DEV;
				}
			}
			if (p->bearer)
				p->bearer->inalarm = 1;
			else
#endif
			p->inalarm = 1;
			res = get_alarms(p);
			handle_alarms(p, res);
#ifdef HAVE_PRI
			if (!p->pri || !p->pri->pri || pri_get_timer(p->pri->pri, PRI_TIMER_T309) < 0) {
				/* fall through intentionally */
			} else {
				break;
			}
#endif
#ifdef HAVE_SS7
			if (p->sig == SIG_SS7)
				break;
#endif
		case DAHDI_EVENT_ONHOOK:
			if (p->radio) {
				p->subs[index].f.frametype = AST_FRAME_CONTROL;
				p->subs[index].f.subclass = AST_CONTROL_RADIO_UNKEY;
				break;
			}
			if (p->oprmode < 0)
			{
				if (p->oprmode != -1) break;
				if ((p->sig == SIG_FXOLS) || (p->sig == SIG_FXOKS) || (p->sig == SIG_FXOGS))
				{
					/* Make sure it starts ringing */
					dahdi_set_hook(p->subs[SUB_REAL].dfd, DAHDI_RINGOFF);
					dahdi_set_hook(p->subs[SUB_REAL].dfd, DAHDI_RING);
					save_conference(p->oprpeer);
					tone_zone_play_tone(p->oprpeer->subs[SUB_REAL].dfd, DAHDI_TONE_RINGTONE);
				}
				break;
			}
			switch (p->sig) {
			case SIG_FXOLS:
			case SIG_FXOGS:
			case SIG_FXOKS:
				p->onhooktime = time(NULL);
				p->msgstate = -1;
				/* Check for some special conditions regarding call waiting */
				if (index == SUB_REAL) {
					/* The normal line was hung up */
					if (p->subs[SUB_CALLWAIT].owner) {
						/* There's a call waiting call, so ring the phone, but make it unowned in the mean time */
						swap_subs(p, SUB_CALLWAIT, SUB_REAL);
						ast_verb(3, "Channel %d still has (callwait) call, ringing phone\n", p->channel);
						unalloc_sub(p, SUB_CALLWAIT);
#if 0
						p->subs[index].needanswer = 0;
						p->subs[index].needringing = 0;
#endif
						p->callwaitingrepeat = 0;
						p->cidcwexpire = 0;
						p->owner = NULL;
						/* Don't start streaming audio yet if the incoming call isn't up yet */
						if (p->subs[SUB_REAL].owner->_state != AST_STATE_UP)
							p->dialing = 1;
						dahdi_ring_phone(p);
					} else if (p->subs[SUB_THREEWAY].owner) {
						unsigned int mssinceflash;
						/* Here we have to retain the lock on both the main channel, the 3-way channel, and
						   the private structure -- not especially easy or clean */
						while (p->subs[SUB_THREEWAY].owner && ast_channel_trylock(p->subs[SUB_THREEWAY].owner)) {
							/* Yuck, didn't get the lock on the 3-way, gotta release everything and re-grab! */
							ast_mutex_unlock(&p->lock);
							ast_channel_unlock(ast);
							usleep(1);
							/* We can grab ast and p in that order, without worry.  We should make sure
							   nothing seriously bad has happened though like some sort of bizarre double
							   masquerade! */
							ast_channel_lock(ast);
							ast_mutex_lock(&p->lock);
							if (p->owner != ast) {
								ast_log(LOG_WARNING, "This isn't good...\n");
								return NULL;
							}
						}
						if (!p->subs[SUB_THREEWAY].owner) {
							ast_log(LOG_NOTICE, "Whoa, threeway disappeared kinda randomly.\n");
							return NULL;
						}
						mssinceflash = ast_tvdiff_ms(ast_tvnow(), p->flashtime);
						ast_debug(1, "Last flash was %d ms ago\n", mssinceflash);
						if (mssinceflash < MIN_MS_SINCE_FLASH) {
							/* It hasn't been long enough since the last flashook.  This is probably a bounce on
							   hanging up.  Hangup both channels now */
							if (p->subs[SUB_THREEWAY].owner)
								ast_queue_hangup(p->subs[SUB_THREEWAY].owner);
							p->subs[SUB_THREEWAY].owner->_softhangup |= AST_SOFTHANGUP_DEV;
							ast_debug(1, "Looks like a bounced flash, hanging up both calls on %d\n", p->channel);
							ast_channel_unlock(p->subs[SUB_THREEWAY].owner);
						} else if ((ast->pbx) || (ast->_state == AST_STATE_UP)) {
							if (p->transfer) {
								/* In any case this isn't a threeway call anymore */
								p->subs[SUB_REAL].inthreeway = 0;
								p->subs[SUB_THREEWAY].inthreeway = 0;
								/* Only attempt transfer if the phone is ringing; why transfer to busy tone eh? */
								if (!p->transfertobusy && ast->_state == AST_STATE_BUSY) {
									ast_channel_unlock(p->subs[SUB_THREEWAY].owner);
									/* Swap subs and dis-own channel */
									swap_subs(p, SUB_THREEWAY, SUB_REAL);
									p->owner = NULL;
									/* Ring the phone */
									dahdi_ring_phone(p);
								} else {
									if ((res = attempt_transfer(p)) < 0) {
										p->subs[SUB_THREEWAY].owner->_softhangup |= AST_SOFTHANGUP_DEV;
										if (p->subs[SUB_THREEWAY].owner)
											ast_channel_unlock(p->subs[SUB_THREEWAY].owner);
									} else if (res) {
										/* Don't actually hang up at this point */
										if (p->subs[SUB_THREEWAY].owner)
											ast_channel_unlock(p->subs[SUB_THREEWAY].owner);
										break;
									}
								}
							} else {
								p->subs[SUB_THREEWAY].owner->_softhangup |= AST_SOFTHANGUP_DEV;
								if (p->subs[SUB_THREEWAY].owner)
									ast_channel_unlock(p->subs[SUB_THREEWAY].owner);
							}
						} else {
							ast_channel_unlock(p->subs[SUB_THREEWAY].owner);
							/* Swap subs and dis-own channel */
							swap_subs(p, SUB_THREEWAY, SUB_REAL);
							p->owner = NULL;
							/* Ring the phone */
							dahdi_ring_phone(p);
						}
					}
				} else {
					ast_log(LOG_WARNING, "Got a hangup and my index is %d?\n", index);
				}
				/* Fall through */
			default:
				dahdi_disable_ec(p);
				return NULL;
			}
			break;
		case DAHDI_EVENT_RINGOFFHOOK:
			if (p->inalarm) break;
			if (p->oprmode < 0)
			{
				if ((p->sig == SIG_FXOLS) || (p->sig == SIG_FXOKS) || (p->sig == SIG_FXOGS))
				{
					/* Make sure it stops ringing */
					dahdi_set_hook(p->subs[SUB_REAL].dfd, DAHDI_RINGOFF);
					tone_zone_play_tone(p->oprpeer->subs[SUB_REAL].dfd, -1);
					restore_conference(p->oprpeer);
				}
				break;
			}
			if (p->radio)
			{
				p->subs[index].f.frametype = AST_FRAME_CONTROL;
				p->subs[index].f.subclass = AST_CONTROL_RADIO_KEY;
				break;
 			}
			/* for E911, its supposed to wait for offhook then dial
			   the second half of the dial string */
			if (((mysig == SIG_E911) || (mysig == SIG_FGC_CAMA) || (mysig == SIG_FGC_CAMAMF)) && (ast->_state == AST_STATE_DIALING_OFFHOOK)) {
				c = strchr(p->dialdest, '/');
				if (c)
					c++;
				else
					c = p->dialdest;
				if (*c) snprintf(p->dop.dialstr, sizeof(p->dop.dialstr), "M*0%s#", c);
				else ast_copy_string(p->dop.dialstr,"M*2#", sizeof(p->dop.dialstr));
				if (strlen(p->dop.dialstr) > 4) {
					memset(p->echorest, 'w', sizeof(p->echorest) - 1);
					strcpy(p->echorest + (p->echotraining / 401) + 1, p->dop.dialstr + strlen(p->dop.dialstr) - 2);
					p->echorest[sizeof(p->echorest) - 1] = '\0';
					p->echobreak = 1;
					p->dop.dialstr[strlen(p->dop.dialstr)-2] = '\0';
				} else
					p->echobreak = 0;
				if (ioctl(p->subs[SUB_REAL].dfd, DAHDI_DIAL, &p->dop)) {
					int saveerr = errno;

					x = DAHDI_ONHOOK;
					ioctl(p->subs[SUB_REAL].dfd, DAHDI_HOOK, &x);
					ast_log(LOG_WARNING, "Dialing failed on channel %d: %s\n", p->channel, strerror(saveerr));
					return NULL;
					}
				p->dialing = 1;
				return &p->subs[index].f;
			}
			switch (p->sig) {
			case SIG_FXOLS:
			case SIG_FXOGS:
			case SIG_FXOKS:
				switch (ast->_state) {
				case AST_STATE_RINGING:
					dahdi_enable_ec(p);
					dahdi_train_ec(p);
					p->subs[index].f.frametype = AST_FRAME_CONTROL;
					p->subs[index].f.subclass = AST_CONTROL_ANSWER;
					/* Make sure it stops ringing */
					dahdi_set_hook(p->subs[index].dfd, DAHDI_OFFHOOK);
					ast_debug(1, "channel %d answered\n", p->channel);
					if (p->cidspill) {
						/* Cancel any running CallerID spill */
						ast_free(p->cidspill);
						p->cidspill = NULL;
					}
					p->dialing = 0;
					p->callwaitcas = 0;
					if (p->confirmanswer) {
						/* Ignore answer if "confirm answer" is enabled */
						p->subs[index].f.frametype = AST_FRAME_NULL;
						p->subs[index].f.subclass = 0;
					} else if (!ast_strlen_zero(p->dop.dialstr)) {
						/* nick@dccinc.com 4/3/03 - fxo should be able to do deferred dialing */
						res = ioctl(p->subs[SUB_REAL].dfd, DAHDI_DIAL, &p->dop);
						if (res < 0) {
							ast_log(LOG_WARNING, "Unable to initiate dialing on trunk channel %d: %s\n", p->channel, strerror(errno));
							p->dop.dialstr[0] = '\0';
							return NULL;
						} else {
							ast_debug(1, "Sent FXO deferred digit string: %s\n", p->dop.dialstr);
							p->subs[index].f.frametype = AST_FRAME_NULL;
							p->subs[index].f.subclass = 0;
							p->dialing = 1;
						}
						p->dop.dialstr[0] = '\0';
						ast_setstate(ast, AST_STATE_DIALING);
					} else
						ast_setstate(ast, AST_STATE_UP);
					return &p->subs[index].f;
				case AST_STATE_DOWN:
					ast_setstate(ast, AST_STATE_RING);
					ast->rings = 1;
					p->subs[index].f.frametype = AST_FRAME_CONTROL;
					p->subs[index].f.subclass = AST_CONTROL_OFFHOOK;
					ast_debug(1, "channel %d picked up\n", p->channel);
					return &p->subs[index].f;
				case AST_STATE_UP:
					/* Make sure it stops ringing */
					dahdi_set_hook(p->subs[index].dfd, DAHDI_OFFHOOK);
					/* Okay -- probably call waiting*/
					if (ast_bridged_channel(p->owner))
						ast_queue_control(p->owner, AST_CONTROL_UNHOLD);
					p->subs[index].needunhold = 1;
					break;
				case AST_STATE_RESERVED:
					/* Start up dialtone */
					if (has_voicemail(p))
						res = tone_zone_play_tone(p->subs[SUB_REAL].dfd, DAHDI_TONE_STUTTER);
					else
						res = tone_zone_play_tone(p->subs[SUB_REAL].dfd, DAHDI_TONE_DIALTONE);
					break;
				default:
					ast_log(LOG_WARNING, "FXO phone off hook in weird state %d??\n", ast->_state);
				}
				break;
			case SIG_FXSLS:
			case SIG_FXSGS:
			case SIG_FXSKS:
				if (ast->_state == AST_STATE_RING) {
					p->ringt = p->ringt_base;
				}

				/* If we get a ring then we cannot be in
				 * reversed polarity. So we reset to idle */
				ast_debug(1, "Setting IDLE polarity due "
					"to ring. Old polarity was %d\n",
					p->polarity);
				p->polarity = POLARITY_IDLE;

				/* Fall through */
			case SIG_EM:
			case SIG_EM_E1:
			case SIG_EMWINK:
			case SIG_FEATD:
			case SIG_FEATDMF:
			case SIG_FEATDMF_TA:
			case SIG_E911:
			case SIG_FGC_CAMA:
			case SIG_FGC_CAMAMF:
			case SIG_FEATB:
			case SIG_SF:
			case SIG_SFWINK:
			case SIG_SF_FEATD:
			case SIG_SF_FEATDMF:
			case SIG_SF_FEATB:
				if (ast->_state == AST_STATE_PRERING)
					ast_setstate(ast, AST_STATE_RING);
				if ((ast->_state == AST_STATE_DOWN) || (ast->_state == AST_STATE_RING)) {
					ast_debug(1, "Ring detected\n");
					p->subs[index].f.frametype = AST_FRAME_CONTROL;
					p->subs[index].f.subclass = AST_CONTROL_RING;
				} else if (p->outgoing && ((ast->_state == AST_STATE_RINGING) || (ast->_state == AST_STATE_DIALING))) {
					ast_debug(1, "Line answered\n");
					if (p->confirmanswer) {
						p->subs[index].f.frametype = AST_FRAME_NULL;
						p->subs[index].f.subclass = 0;
					} else {
						p->subs[index].f.frametype = AST_FRAME_CONTROL;
						p->subs[index].f.subclass = AST_CONTROL_ANSWER;
						ast_setstate(ast, AST_STATE_UP);
					}
				} else if (ast->_state != AST_STATE_RING)
					ast_log(LOG_WARNING, "Ring/Off-hook in strange state %d on channel %d\n", ast->_state, p->channel);
				break;
			default:
				ast_log(LOG_WARNING, "Don't know how to handle ring/off hook for signalling %d\n", p->sig);
			}
			break;
		case DAHDI_EVENT_RINGBEGIN:
			switch (p->sig) {
			case SIG_FXSLS:
			case SIG_FXSGS:
			case SIG_FXSKS:
				if (ast->_state == AST_STATE_RING) {
					p->ringt = p->ringt_base;
				}
				break;
			}
			break;
		case DAHDI_EVENT_RINGEROFF:
			if (p->inalarm) break;
			if ((p->radio || (p->oprmode < 0))) break;
			ast->rings++;
			if ((ast->rings > p->cidrings) && (p->cidspill)) {
				ast_log(LOG_WARNING, "Didn't finish Caller-ID spill.  Cancelling.\n");
				ast_free(p->cidspill);
				p->cidspill = NULL;
				p->callwaitcas = 0;
			}
			p->subs[index].f.frametype = AST_FRAME_CONTROL;
			p->subs[index].f.subclass = AST_CONTROL_RINGING;
			break;
		case DAHDI_EVENT_RINGERON:
			break;
		case DAHDI_EVENT_NOALARM:
			p->inalarm = 0;
#ifdef HAVE_PRI
			/* Extremely unlikely but just in case */
			if (p->bearer)
				p->bearer->inalarm = 0;
#endif
			ast_log(LOG_NOTICE, "Alarm cleared on channel %d\n", p->channel);
			manager_event(EVENT_FLAG_SYSTEM, "AlarmClear",
								"Channel: %d\r\n", p->channel);
			break;
		case DAHDI_EVENT_WINKFLASH:
			if (p->inalarm) break;
			if (p->radio) break;
			if (p->oprmode < 0) break;
			if (p->oprmode > 1)
			{
				struct dahdi_params par;

				if (ioctl(p->oprpeer->subs[SUB_REAL].dfd, DAHDI_GET_PARAMS, &par) != -1)
				{
					if (!par.rxisoffhook)
					{
						/* Make sure it stops ringing */
						dahdi_set_hook(p->oprpeer->subs[SUB_REAL].dfd, DAHDI_RINGOFF);
						dahdi_set_hook(p->oprpeer->subs[SUB_REAL].dfd, DAHDI_RING);
						save_conference(p);
						tone_zone_play_tone(p->subs[SUB_REAL].dfd, DAHDI_TONE_RINGTONE);
					}
				}
				break;
			}
			/* Remember last time we got a flash-hook */
			p->flashtime = ast_tvnow();
			switch (mysig) {
			case SIG_FXOLS:
			case SIG_FXOGS:
			case SIG_FXOKS:
				ast_debug(1, "Winkflash, index: %d, normal: %d, callwait: %d, thirdcall: %d\n",
					index, p->subs[SUB_REAL].dfd, p->subs[SUB_CALLWAIT].dfd, p->subs[SUB_THREEWAY].dfd);
				p->callwaitcas = 0;

				if (index != SUB_REAL) {
					ast_log(LOG_WARNING, "Got flash hook with index %d on channel %d?!?\n", index, p->channel);
					goto winkflashdone;
				}

				if (p->subs[SUB_CALLWAIT].owner) {
					/* Swap to call-wait */
					swap_subs(p, SUB_REAL, SUB_CALLWAIT);
					tone_zone_play_tone(p->subs[SUB_REAL].dfd, -1);
					p->owner = p->subs[SUB_REAL].owner;
					ast_debug(1, "Making %s the new owner\n", p->owner->name);
					if (p->owner->_state == AST_STATE_RINGING) {
						ast_setstate(p->owner, AST_STATE_UP);
						p->subs[SUB_REAL].needanswer = 1;
					}
					p->callwaitingrepeat = 0;
					p->cidcwexpire = 0;
					/* Start music on hold if appropriate */
					if (!p->subs[SUB_CALLWAIT].inthreeway && ast_bridged_channel(p->subs[SUB_CALLWAIT].owner)) {
						ast_queue_control_data(p->subs[SUB_CALLWAIT].owner, AST_CONTROL_HOLD,
							S_OR(p->mohsuggest, NULL),
							!ast_strlen_zero(p->mohsuggest) ? strlen(p->mohsuggest) + 1 : 0);
					}
					p->subs[SUB_CALLWAIT].needhold = 1;
					if (ast_bridged_channel(p->subs[SUB_REAL].owner)) {
						ast_queue_control_data(p->subs[SUB_REAL].owner, AST_CONTROL_HOLD,
							S_OR(p->mohsuggest, NULL),
							!ast_strlen_zero(p->mohsuggest) ? strlen(p->mohsuggest) + 1 : 0);
					}
					p->subs[SUB_REAL].needunhold = 1;
				} else if (!p->subs[SUB_THREEWAY].owner) {
					char cid_num[256];
					char cid_name[256];

					if (!p->threewaycalling) {
						/* Just send a flash if no 3-way calling */
						p->subs[SUB_REAL].needflash = 1;
						goto winkflashdone;
					} else if (!check_for_conference(p)) {
						if (p->dahditrcallerid && p->owner) {
							if (p->owner->cid.cid_num)
								ast_copy_string(cid_num, p->owner->cid.cid_num, sizeof(cid_num));
							if (p->owner->cid.cid_name)
								ast_copy_string(cid_name, p->owner->cid.cid_name, sizeof(cid_name));
						}
						/* XXX This section needs much more error checking!!! XXX */
						/* Start a 3-way call if feasible */
						if (!((ast->pbx) ||
						      (ast->_state == AST_STATE_UP) ||
						      (ast->_state == AST_STATE_RING))) {
							ast_debug(1, "Flash when call not up or ringing\n");
							goto winkflashdone;
						}
						if (alloc_sub(p, SUB_THREEWAY)) {
							ast_log(LOG_WARNING, "Unable to allocate three-way subchannel\n");
							goto winkflashdone;
						}
						/* Make new channel */
						chan = dahdi_new(p, AST_STATE_RESERVED, 0, SUB_THREEWAY, 0, 0);
						if (p->dahditrcallerid) {
							if (!p->origcid_num)
								p->origcid_num = ast_strdup(p->cid_num);
							if (!p->origcid_name)
								p->origcid_name = ast_strdup(p->cid_name);
							ast_copy_string(p->cid_num, cid_num, sizeof(p->cid_num));
							ast_copy_string(p->cid_name, cid_name, sizeof(p->cid_name));
						}
						/* Swap things around between the three-way and real call */
						swap_subs(p, SUB_THREEWAY, SUB_REAL);
						/* Disable echo canceller for better dialing */
						dahdi_disable_ec(p);
						res = tone_zone_play_tone(p->subs[SUB_REAL].dfd, DAHDI_TONE_DIALRECALL);
						if (res)
							ast_log(LOG_WARNING, "Unable to start dial recall tone on channel %d\n", p->channel);
						p->owner = chan;
						if (!chan) {
							ast_log(LOG_WARNING, "Cannot allocate new structure on channel %d\n", p->channel);
						} else if (ast_pthread_create_detached(&threadid, NULL, ss_thread, chan)) {
							ast_log(LOG_WARNING, "Unable to start simple switch on channel %d\n", p->channel);
							res = tone_zone_play_tone(p->subs[SUB_REAL].dfd, DAHDI_TONE_CONGESTION);
							dahdi_enable_ec(p);
							ast_hangup(chan);
						} else {
 							struct ast_channel *other = ast_bridged_channel(p->subs[SUB_THREEWAY].owner);
 							int way3bridge = 0, cdr3way = 0;

 							if (!other) {
 								other = ast_bridged_channel(p->subs[SUB_REAL].owner);
 							} else
 								way3bridge = 1;

 							if (p->subs[SUB_THREEWAY].owner->cdr)
 								cdr3way = 1;

							ast_verb(3, "Started three way call on channel %d\n", p->channel);

							/* Start music on hold if appropriate */
							if (ast_bridged_channel(p->subs[SUB_THREEWAY].owner)) {
								ast_queue_control_data(p->subs[SUB_THREEWAY].owner, AST_CONTROL_HOLD,
									S_OR(p->mohsuggest, NULL),
									!ast_strlen_zero(p->mohsuggest) ? strlen(p->mohsuggest) + 1 : 0);
							}
							p->subs[SUB_THREEWAY].needhold = 1;
						}
					}
				} else {
					/* Already have a 3 way call */
					if (p->subs[SUB_THREEWAY].inthreeway) {
						/* Call is already up, drop the last person */
						ast_debug(1, "Got flash with three way call up, dropping last call on %d\n", p->channel);
						/* If the primary call isn't answered yet, use it */
						if ((p->subs[SUB_REAL].owner->_state != AST_STATE_UP) && (p->subs[SUB_THREEWAY].owner->_state == AST_STATE_UP)) {
							/* Swap back -- we're dropping the real 3-way that isn't finished yet*/
							swap_subs(p, SUB_THREEWAY, SUB_REAL);
							p->owner = p->subs[SUB_REAL].owner;
						}
						/* Drop the last call and stop the conference */
						ast_verb(3, "Dropping three-way call on %s\n", p->subs[SUB_THREEWAY].owner->name);
						p->subs[SUB_THREEWAY].owner->_softhangup |= AST_SOFTHANGUP_DEV;
						p->subs[SUB_REAL].inthreeway = 0;
						p->subs[SUB_THREEWAY].inthreeway = 0;
					} else {
						/* Lets see what we're up to */
						if (((ast->pbx) || (ast->_state == AST_STATE_UP)) &&
						    (p->transfertobusy || (ast->_state != AST_STATE_BUSY))) {
							int otherindex = SUB_THREEWAY;
							struct ast_channel *other = ast_bridged_channel(p->subs[SUB_THREEWAY].owner);
							int way3bridge = 0, cdr3way = 0;

							if (!other) {
								other = ast_bridged_channel(p->subs[SUB_REAL].owner);
							} else
								way3bridge = 1;

							if (p->subs[SUB_THREEWAY].owner->cdr)
								cdr3way = 1;

							ast_verb(3, "Building conference on call on %s and %s\n", p->subs[SUB_THREEWAY].owner->name, p->subs[SUB_REAL].owner->name);
							/* Put them in the threeway, and flip */
							p->subs[SUB_THREEWAY].inthreeway = 1;
							p->subs[SUB_REAL].inthreeway = 1;
							if (ast->_state == AST_STATE_UP) {
								swap_subs(p, SUB_THREEWAY, SUB_REAL);
								otherindex = SUB_REAL;
							}
							if (p->subs[otherindex].owner && ast_bridged_channel(p->subs[otherindex].owner))
								ast_queue_control(p->subs[otherindex].owner, AST_CONTROL_UNHOLD);
							p->subs[otherindex].needunhold = 1;
							p->owner = p->subs[SUB_REAL].owner;
							if (ast->_state == AST_STATE_RINGING) {
								ast_debug(1, "Enabling ringtone on real and threeway\n");
								res = tone_zone_play_tone(p->subs[SUB_REAL].dfd, DAHDI_TONE_RINGTONE);
								res = tone_zone_play_tone(p->subs[SUB_THREEWAY].dfd, DAHDI_TONE_RINGTONE);
							}
						} else {
							ast_verb(3, "Dumping incomplete call on on %s\n", p->subs[SUB_THREEWAY].owner->name);
							swap_subs(p, SUB_THREEWAY, SUB_REAL);
							p->subs[SUB_THREEWAY].owner->_softhangup |= AST_SOFTHANGUP_DEV;
							p->owner = p->subs[SUB_REAL].owner;
							if (p->subs[SUB_REAL].owner && ast_bridged_channel(p->subs[SUB_REAL].owner))
								ast_queue_control(p->subs[SUB_REAL].owner, AST_CONTROL_UNHOLD);
							p->subs[SUB_REAL].needunhold = 1;
							dahdi_enable_ec(p);
						}

					}
				}
			winkflashdone:
				update_conf(p);
				break;
			case SIG_EM:
			case SIG_EM_E1:
			case SIG_EMWINK:
			case SIG_FEATD:
			case SIG_SF:
			case SIG_SFWINK:
			case SIG_SF_FEATD:
			case SIG_FXSLS:
			case SIG_FXSGS:
				if (option_debug) {
					if (p->dialing)
						ast_debug(1, "Ignoring wink on channel %d\n", p->channel);
					else
						ast_debug(1, "Got wink in weird state %d on channel %d\n", ast->_state, p->channel);
				}
				break;
			case SIG_FEATDMF_TA:
				switch (p->whichwink) {
				case 0:
					ast_debug(1, "ANI2 set to '%d' and ANI is '%s'\n", p->owner->cid.cid_ani2, p->owner->cid.cid_ani);
					snprintf(p->dop.dialstr, sizeof(p->dop.dialstr), "M*%d%s#", p->owner->cid.cid_ani2, p->owner->cid.cid_ani);
					break;
				case 1:
					ast_copy_string(p->dop.dialstr, p->finaldial, sizeof(p->dop.dialstr));
					break;
				case 2:
					ast_log(LOG_WARNING, "Received unexpected wink on channel of type SIG_FEATDMF_TA\n");
					return NULL;
				}
				p->whichwink++;
				/* Fall through */
			case SIG_FEATDMF:
			case SIG_E911:
			case SIG_FGC_CAMAMF:
			case SIG_FGC_CAMA:
			case SIG_FEATB:
			case SIG_SF_FEATDMF:
			case SIG_SF_FEATB:
				/* FGD MF *Must* wait for wink */
				if (!ast_strlen_zero(p->dop.dialstr)) {
					res = ioctl(p->subs[SUB_REAL].dfd, DAHDI_DIAL, &p->dop);
					if (res < 0) {
						ast_log(LOG_WARNING, "Unable to initiate dialing on trunk channel %d: %s\n", p->channel, strerror(errno));
						p->dop.dialstr[0] = '\0';
						return NULL;
					} else
						ast_debug(1, "Sent deferred digit string: %s\n", p->dop.dialstr);
				}
				p->dop.dialstr[0] = '\0';
				break;
			default:
				ast_log(LOG_WARNING, "Don't know how to handle ring/off hoook for signalling %d\n", p->sig);
			}
			break;
		case DAHDI_EVENT_HOOKCOMPLETE:
			if (p->inalarm) break;
			if ((p->radio || (p->oprmode < 0))) break;
			switch (mysig) {
			case SIG_FXSLS:  /* only interesting for FXS */
			case SIG_FXSGS:
			case SIG_FXSKS:
			case SIG_EM:
			case SIG_EM_E1:
			case SIG_EMWINK:
			case SIG_FEATD:
			case SIG_SF:
			case SIG_SFWINK:
			case SIG_SF_FEATD:
				if (!ast_strlen_zero(p->dop.dialstr)) {
					res = ioctl(p->subs[SUB_REAL].dfd, DAHDI_DIAL, &p->dop);
					if (res < 0) {
						ast_log(LOG_WARNING, "Unable to initiate dialing on trunk channel %d: %s\n", p->channel, strerror(errno));
						p->dop.dialstr[0] = '\0';
						return NULL;
					} else
						ast_debug(1, "Sent deferred digit string: %s\n", p->dop.dialstr);
				}
				p->dop.dialstr[0] = '\0';
				p->dop.op = DAHDI_DIAL_OP_REPLACE;
				break;
			case SIG_FEATDMF:
			case SIG_FEATDMF_TA:
			case SIG_E911:
			case SIG_FGC_CAMA:
			case SIG_FGC_CAMAMF:
			case SIG_FEATB:
			case SIG_SF_FEATDMF:
			case SIG_SF_FEATB:
				ast_debug(1, "Got hook complete in MF FGD, waiting for wink now on channel %d\n",p->channel);
				break;
			default:
				break;
			}
			break;
		case DAHDI_EVENT_POLARITY:
			/*
			 * If we get a Polarity Switch event, check to see
			 * if we should change the polarity state and
			 * mark the channel as UP or if this is an indication
			 * of remote end disconnect.
			 */
			if (p->polarity == POLARITY_IDLE) {
				p->polarity = POLARITY_REV;
				if (p->answeronpolarityswitch &&
				    ((ast->_state == AST_STATE_DIALING) ||
					 (ast->_state == AST_STATE_RINGING))) {
					ast_debug(1, "Answering on polarity switch!\n");
					ast_setstate(p->owner, AST_STATE_UP);
					if (p->hanguponpolarityswitch) {
						p->polaritydelaytv = ast_tvnow();
					}
				} else
					ast_debug(1, "Ignore switch to REVERSED Polarity on channel %d, state %d\n", p->channel, ast->_state);

			}
			/* Removed else statement from here as it was preventing hangups from ever happening*/
			/* Added AST_STATE_RING in if statement below to deal with calling party hangups that take place when ringing */
			if (p->hanguponpolarityswitch &&
				(p->polarityonanswerdelay > 0) &&
			       (p->polarity == POLARITY_REV) &&
				((ast->_state == AST_STATE_UP) || (ast->_state == AST_STATE_RING)) ) {
                                /* Added log_debug information below to provide a better indication of what is going on */
				ast_debug(1, "Polarity Reversal event occured - DEBUG 1: channel %d, state %d, pol= %d, aonp= %d, honp= %d, pdelay= %d, tv= %d\n", p->channel, ast->_state, p->polarity, p->answeronpolarityswitch, p->hanguponpolarityswitch, p->polarityonanswerdelay, ast_tvdiff_ms(ast_tvnow(), p->polaritydelaytv) );

				if (ast_tvdiff_ms(ast_tvnow(), p->polaritydelaytv) > p->polarityonanswerdelay) {
					ast_debug(1, "Polarity Reversal detected and now Hanging up on channel %d\n", p->channel);
					ast_softhangup(p->owner, AST_SOFTHANGUP_EXPLICIT);
					p->polarity = POLARITY_IDLE;
				} else
					ast_debug(1, "Polarity Reversal detected but NOT hanging up (too close to answer event) on channel %d, state %d\n", p->channel, ast->_state);

			} else {
				p->polarity = POLARITY_IDLE;
				ast_debug(1, "Ignoring Polarity switch to IDLE on channel %d, state %d\n", p->channel, ast->_state);
			}
                     	/* Added more log_debug information below to provide a better indication of what is going on */
			ast_debug(1, "Polarity Reversal event occured - DEBUG 2: channel %d, state %d, pol= %d, aonp= %d, honp= %d, pdelay= %d, tv= %d\n", p->channel, ast->_state, p->polarity, p->answeronpolarityswitch, p->hanguponpolarityswitch, p->polarityonanswerdelay, ast_tvdiff_ms(ast_tvnow(), p->polaritydelaytv) );
			break;
		default:
			ast_debug(1, "Dunno what to do with event %d on channel %d\n", res, p->channel);
	}
	return &p->subs[index].f;
}

static struct ast_frame *__dahdi_exception(struct ast_channel *ast)
{
	struct dahdi_pvt *p = ast->tech_pvt;
	int res;
	int usedindex=-1;
	int index;
	struct ast_frame *f;


	index = dahdi_get_index(ast, p, 1);

	p->subs[index].f.frametype = AST_FRAME_NULL;
	p->subs[index].f.datalen = 0;
	p->subs[index].f.samples = 0;
	p->subs[index].f.mallocd = 0;
	p->subs[index].f.offset = 0;
	p->subs[index].f.subclass = 0;
	p->subs[index].f.delivery = ast_tv(0,0);
	p->subs[index].f.src = "dahdi_exception";
	p->subs[index].f.data = NULL;


	if ((!p->owner) && (!(p->radio || (p->oprmode < 0)))) {
		/* If nobody owns us, absorb the event appropriately, otherwise
		   we loop indefinitely.  This occurs when, during call waiting, the
		   other end hangs up our channel so that it no longer exists, but we
		   have neither FLASH'd nor ONHOOK'd to signify our desire to
		   change to the other channel. */
		if (p->fake_event) {
			res = p->fake_event;
			p->fake_event = 0;
		} else
			res = dahdi_get_event(p->subs[SUB_REAL].dfd);
		/* Switch to real if there is one and this isn't something really silly... */
		if ((res != DAHDI_EVENT_RINGEROFF) && (res != DAHDI_EVENT_RINGERON) &&
			(res != DAHDI_EVENT_HOOKCOMPLETE)) {
			ast_debug(1, "Restoring owner of channel %d on event %d\n", p->channel, res);
			p->owner = p->subs[SUB_REAL].owner;
			if (p->owner && ast_bridged_channel(p->owner))
				ast_queue_control(p->owner, AST_CONTROL_UNHOLD);
			p->subs[SUB_REAL].needunhold = 1;
		}
		switch (res) {
		case DAHDI_EVENT_ONHOOK:
			dahdi_disable_ec(p);
			if (p->owner) {
				ast_verb(3, "Channel %s still has call, ringing phone\n", p->owner->name);
				dahdi_ring_phone(p);
				p->callwaitingrepeat = 0;
				p->cidcwexpire = 0;
			} else
				ast_log(LOG_WARNING, "Absorbed on hook, but nobody is left!?!?\n");
			update_conf(p);
			break;
		case DAHDI_EVENT_RINGOFFHOOK:
			dahdi_enable_ec(p);
			dahdi_set_hook(p->subs[SUB_REAL].dfd, DAHDI_OFFHOOK);
			if (p->owner && (p->owner->_state == AST_STATE_RINGING)) {
				p->subs[SUB_REAL].needanswer = 1;
				p->dialing = 0;
			}
			break;
		case DAHDI_EVENT_HOOKCOMPLETE:
		case DAHDI_EVENT_RINGERON:
		case DAHDI_EVENT_RINGEROFF:
			/* Do nothing */
			break;
		case DAHDI_EVENT_WINKFLASH:
			p->flashtime = ast_tvnow();
			if (p->owner) {
				ast_verb(3, "Channel %d flashed to other channel %s\n", p->channel, p->owner->name);
				if (p->owner->_state != AST_STATE_UP) {
					/* Answer if necessary */
					usedindex = dahdi_get_index(p->owner, p, 0);
					if (usedindex > -1) {
						p->subs[usedindex].needanswer = 1;
					}
					ast_setstate(p->owner, AST_STATE_UP);
				}
				p->callwaitingrepeat = 0;
				p->cidcwexpire = 0;
				if (ast_bridged_channel(p->owner))
					ast_queue_control(p->owner, AST_CONTROL_UNHOLD);
				p->subs[SUB_REAL].needunhold = 1;
			} else
				ast_log(LOG_WARNING, "Absorbed on hook, but nobody is left!?!?\n");
			update_conf(p);
			break;
		default:
			ast_log(LOG_WARNING, "Don't know how to absorb event %s\n", event2str(res));
		}
		f = &p->subs[index].f;
		return f;
	}
	if (!(p->radio || (p->oprmode < 0)))
		ast_debug(1, "Exception on %d, channel %d\n", ast->fds[0],p->channel);
	/* If it's not us, return NULL immediately */
	if (ast != p->owner) {
		ast_log(LOG_WARNING, "We're %s, not %s\n", ast->name, p->owner->name);
		f = &p->subs[index].f;
		return f;
	}
	f = dahdi_handle_event(ast);
	return f;
}

static struct ast_frame *dahdi_exception(struct ast_channel *ast)
{
	struct dahdi_pvt *p = ast->tech_pvt;
	struct ast_frame *f;
	ast_mutex_lock(&p->lock);
	f = __dahdi_exception(ast);
	ast_mutex_unlock(&p->lock);
	return f;
}

static struct ast_frame  *dahdi_read(struct ast_channel *ast)
{
	struct dahdi_pvt *p = ast->tech_pvt;
	int res;
	int index;
	void *readbuf;
	struct ast_frame *f;

	while (ast_mutex_trylock(&p->lock)) {
		CHANNEL_DEADLOCK_AVOIDANCE(ast);
	}

	index = dahdi_get_index(ast, p, 0);

	/* Hang up if we don't really exist */
	if (index < 0)	{
		ast_log(LOG_WARNING, "We dont exist?\n");
		ast_mutex_unlock(&p->lock);
		return NULL;
	}

	if ((p->radio || (p->oprmode < 0)) && p->inalarm) return NULL;

	p->subs[index].f.frametype = AST_FRAME_NULL;
	p->subs[index].f.datalen = 0;
	p->subs[index].f.samples = 0;
	p->subs[index].f.mallocd = 0;
	p->subs[index].f.offset = 0;
	p->subs[index].f.subclass = 0;
	p->subs[index].f.delivery = ast_tv(0,0);
	p->subs[index].f.src = "dahdi_read";
	p->subs[index].f.data = NULL;

	/* make sure it sends initial key state as first frame */
	if ((p->radio || (p->oprmode < 0)) && (!p->firstradio))
	{
		struct dahdi_params ps;

		memset(&ps, 0, sizeof(ps));
		ps.channo = p->channel;
		if (ioctl(p->subs[SUB_REAL].dfd, DAHDI_GET_PARAMS, &ps) < 0) {
			ast_mutex_unlock(&p->lock);
			return NULL;
		}
		p->firstradio = 1;
		p->subs[index].f.frametype = AST_FRAME_CONTROL;
		if (ps.rxisoffhook)
		{
			p->subs[index].f.subclass = AST_CONTROL_RADIO_KEY;
		}
		else
		{
			p->subs[index].f.subclass = AST_CONTROL_RADIO_UNKEY;
		}
		ast_mutex_unlock(&p->lock);
		return &p->subs[index].f;
	}
	if (p->ringt == 1) {
		ast_mutex_unlock(&p->lock);
		return NULL;
	}
	else if (p->ringt > 0)
		p->ringt--;

	if (p->subs[index].needringing) {
		/* Send ringing frame if requested */
		p->subs[index].needringing = 0;
		p->subs[index].f.frametype = AST_FRAME_CONTROL;
		p->subs[index].f.subclass = AST_CONTROL_RINGING;
		ast_setstate(ast, AST_STATE_RINGING);
		ast_mutex_unlock(&p->lock);
		return &p->subs[index].f;
	}

	if (p->subs[index].needbusy) {
		/* Send busy frame if requested */
		p->subs[index].needbusy = 0;
		p->subs[index].f.frametype = AST_FRAME_CONTROL;
		p->subs[index].f.subclass = AST_CONTROL_BUSY;
		ast_mutex_unlock(&p->lock);
		return &p->subs[index].f;
	}

	if (p->subs[index].needcongestion) {
		/* Send congestion frame if requested */
		p->subs[index].needcongestion = 0;
		p->subs[index].f.frametype = AST_FRAME_CONTROL;
		p->subs[index].f.subclass = AST_CONTROL_CONGESTION;
		ast_mutex_unlock(&p->lock);
		return &p->subs[index].f;
	}

	if (p->subs[index].needcallerid) {
		ast_set_callerid(ast, S_OR(p->lastcid_num, NULL),
							S_OR(p->lastcid_name, NULL),
							S_OR(p->lastcid_num, NULL)
							);
		p->subs[index].needcallerid = 0;
	}

	if (p->subs[index].needanswer) {
		/* Send answer frame if requested */
		p->subs[index].needanswer = 0;
		p->subs[index].f.frametype = AST_FRAME_CONTROL;
		p->subs[index].f.subclass = AST_CONTROL_ANSWER;
		ast_mutex_unlock(&p->lock);
		return &p->subs[index].f;
	}

	if (p->subs[index].needflash) {
		/* Send answer frame if requested */
		p->subs[index].needflash = 0;
		p->subs[index].f.frametype = AST_FRAME_CONTROL;
		p->subs[index].f.subclass = AST_CONTROL_FLASH;
		ast_mutex_unlock(&p->lock);
		return &p->subs[index].f;
	}

	if (p->subs[index].needhold) {
		/* Send answer frame if requested */
		p->subs[index].needhold = 0;
		p->subs[index].f.frametype = AST_FRAME_CONTROL;
		p->subs[index].f.subclass = AST_CONTROL_HOLD;
		ast_mutex_unlock(&p->lock);
		ast_debug(1, "Sending hold on '%s'\n", ast->name);
		return &p->subs[index].f;
	}

	if (p->subs[index].needunhold) {
		/* Send answer frame if requested */
		p->subs[index].needunhold = 0;
		p->subs[index].f.frametype = AST_FRAME_CONTROL;
		p->subs[index].f.subclass = AST_CONTROL_UNHOLD;
		ast_mutex_unlock(&p->lock);
		ast_debug(1, "Sending unhold on '%s'\n", ast->name);
		return &p->subs[index].f;
	}

	if (ast->rawreadformat == AST_FORMAT_SLINEAR) {
		if (!p->subs[index].linear) {
			p->subs[index].linear = 1;
			res = dahdi_setlinear(p->subs[index].dfd, p->subs[index].linear);
			if (res)
				ast_log(LOG_WARNING, "Unable to set channel %d (index %d) to linear mode.\n", p->channel, index);
		}
	} else if ((ast->rawreadformat == AST_FORMAT_ULAW) ||
		   (ast->rawreadformat == AST_FORMAT_ALAW)) {
		if (p->subs[index].linear) {
			p->subs[index].linear = 0;
			res = dahdi_setlinear(p->subs[index].dfd, p->subs[index].linear);
			if (res)
				ast_log(LOG_WARNING, "Unable to set channel %d (index %d) to companded mode.\n", p->channel, index);
		}
	} else {
		ast_log(LOG_WARNING, "Don't know how to read frames in format %s\n", ast_getformatname(ast->rawreadformat));
		ast_mutex_unlock(&p->lock);
		return NULL;
	}
	readbuf = ((unsigned char *)p->subs[index].buffer) + AST_FRIENDLY_OFFSET;
	CHECK_BLOCKING(ast);
	res = read(p->subs[index].dfd, readbuf, p->subs[index].linear ? READ_SIZE * 2 : READ_SIZE);
	ast_clear_flag(ast, AST_FLAG_BLOCKING);
	/* Check for hangup */
	if (res < 0) {
		f = NULL;
		if (res == -1)  {
			if (errno == EAGAIN) {
				/* Return "NULL" frame if there is nobody there */
				ast_mutex_unlock(&p->lock);
				return &p->subs[index].f;
			} else if (errno == ELAST) {
				f = __dahdi_exception(ast);
			} else
				ast_log(LOG_WARNING, "dahdi_rec: %s\n", strerror(errno));
		}
		ast_mutex_unlock(&p->lock);
		return f;
	}
	if (res != (p->subs[index].linear ? READ_SIZE * 2 : READ_SIZE)) {
		ast_debug(1, "Short read (%d/%d), must be an event...\n", res, p->subs[index].linear ? READ_SIZE * 2 : READ_SIZE);
		f = __dahdi_exception(ast);
		ast_mutex_unlock(&p->lock);
		return f;
	}
	if (p->tdd) { /* if in TDD mode, see if we receive that */
		int c;

		c = tdd_feed(p->tdd,readbuf,READ_SIZE);
		if (c < 0) {
			ast_debug(1,"tdd_feed failed\n");
			ast_mutex_unlock(&p->lock);
			return NULL;
		}
		if (c) { /* if a char to return */
			p->subs[index].f.subclass = 0;
			p->subs[index].f.frametype = AST_FRAME_TEXT;
			p->subs[index].f.mallocd = 0;
			p->subs[index].f.offset = AST_FRIENDLY_OFFSET;
			p->subs[index].f.data = p->subs[index].buffer + AST_FRIENDLY_OFFSET;
			p->subs[index].f.datalen = 1;
			*((char *) p->subs[index].f.data) = c;
			ast_mutex_unlock(&p->lock);
			return &p->subs[index].f;
		}
	}
	/* Ensure the CW timer decrements only on a single subchannel */
	if (p->callwaitingrepeat && dahdi_get_index(ast, p, 1) == SUB_REAL) {
		p->callwaitingrepeat--;
	}
	if (p->cidcwexpire)
		p->cidcwexpire--;
	/* Repeat callwaiting */
	if (p->callwaitingrepeat == 1) {
		p->callwaitrings++;
		dahdi_callwait(ast);
	}
	/* Expire CID/CW */
	if (p->cidcwexpire == 1) {
		ast_verb(3, "CPE does not support Call Waiting Caller*ID.\n");
		restore_conference(p);
	}
	if (p->subs[index].linear) {
		p->subs[index].f.datalen = READ_SIZE * 2;
	} else
		p->subs[index].f.datalen = READ_SIZE;

	/* Handle CallerID Transmission */
	if ((p->owner == ast) && p->cidspill &&((ast->_state == AST_STATE_UP) || (ast->rings == p->cidrings))) {
		send_callerid(p);
	}

	p->subs[index].f.frametype = AST_FRAME_VOICE;
	p->subs[index].f.subclass = ast->rawreadformat;
	p->subs[index].f.samples = READ_SIZE;
	p->subs[index].f.mallocd = 0;
	p->subs[index].f.offset = AST_FRIENDLY_OFFSET;
	p->subs[index].f.data = p->subs[index].buffer + AST_FRIENDLY_OFFSET / sizeof(p->subs[index].buffer[0]);
#if 0
	ast_debug(1, "Read %d of voice on %s\n", p->subs[index].f.datalen, ast->name);
#endif
	if (p->dialing || /* Transmitting something */
	   (index && (ast->_state != AST_STATE_UP)) || /* Three-way or callwait that isn't up */
	   ((index == SUB_CALLWAIT) && !p->subs[SUB_CALLWAIT].inthreeway) /* Inactive and non-confed call-wait */
	   ) {
		/* Whoops, we're still dialing, or in a state where we shouldn't transmit....
		   don't send anything */
		p->subs[index].f.frametype = AST_FRAME_NULL;
		p->subs[index].f.subclass = 0;
		p->subs[index].f.samples = 0;
		p->subs[index].f.mallocd = 0;
		p->subs[index].f.offset = 0;
		p->subs[index].f.data = NULL;
		p->subs[index].f.datalen= 0;
	}
	if (p->dsp && (!p->ignoredtmf || p->callwaitcas || p->busydetect  || p->callprogress) && !index) {
		/* Perform busy detection. etc on the dahdi line */
		f = ast_dsp_process(ast, p->dsp, &p->subs[index].f);
		if (f) {
			if ((f->frametype == AST_FRAME_CONTROL) && (f->subclass == AST_CONTROL_BUSY)) {
				if ((ast->_state == AST_STATE_UP) && !p->outgoing) {
					/* Treat this as a "hangup" instead of a "busy" on the assumption that
					   a busy  */
					f = NULL;
				}
			} else if (f->frametype == AST_FRAME_DTMF) {
#ifdef HAVE_PRI
				if (!p->proceeding && ((p->sig == SIG_PRI) || (p->sig == SIG_BRI) || (p->sig == SIG_BRI_PTMP)) && p->pri &&
				    ((!p->outgoing && (p->pri->overlapdial & DAHDI_OVERLAPDIAL_INCOMING)) ||
				     (p->outgoing && (p->pri->overlapdial & DAHDI_OVERLAPDIAL_OUTGOING)))) {
					/* Don't accept in-band DTMF when in overlap dial mode */
					f->frametype = AST_FRAME_NULL;
					f->subclass = 0;
				}
#endif
				/* DSP clears us of being pulse */
				p->pulsedial = 0;
			}
		}
	} else
		f = &p->subs[index].f;

	if (f && (f->frametype == AST_FRAME_DTMF))
		dahdi_handle_dtmfup(ast, index, &f);

	/* If we have a fake_event, trigger exception to handle it */
	if (p->fake_event)
		ast_set_flag(ast, AST_FLAG_EXCEPTION);

	ast_mutex_unlock(&p->lock);
	return f;
}

static int my_dahdi_write(struct dahdi_pvt *p, unsigned char *buf, int len, int index, int linear)
{
	int sent=0;
	int size;
	int res;
	int fd;
	fd = p->subs[index].dfd;
	while (len) {
		size = len;
		if (size > (linear ? READ_SIZE * 2 : READ_SIZE))
			size = (linear ? READ_SIZE * 2 : READ_SIZE);
		res = write(fd, buf, size);
		if (res != size) {
			ast_debug(1, "Write returned %d (%s) on channel %d\n", res, strerror(errno), p->channel);
			return sent;
		}
		len -= size;
		buf += size;
	}
	return sent;
}

static int dahdi_write(struct ast_channel *ast, struct ast_frame *frame)
{
	struct dahdi_pvt *p = ast->tech_pvt;
	int res;
	int index;
	index = dahdi_get_index(ast, p, 0);
	if (index < 0) {
		ast_log(LOG_WARNING, "%s doesn't really exist?\n", ast->name);
		return -1;
	}

#if 0
#ifdef HAVE_PRI
	ast_mutex_lock(&p->lock);
	if (!p->proceeding && p->sig==SIG_PRI && p->pri && !p->outgoing) {
		if (p->pri->pri) {
			if (!pri_grab(p, p->pri)) {
					pri_progress(p->pri->pri,p->call, PVT_TO_CHANNEL(p), !p->digital);
					pri_rel(p->pri);
			} else
					ast_log(LOG_WARNING, "Unable to grab PRI on span %d\n", p->span);
		}
		p->proceeding=1;
		p->dialing = 0;
	}
	ast_mutex_unlock(&p->lock);
#endif
#endif
	/* Write a frame of (presumably voice) data */
	if (frame->frametype != AST_FRAME_VOICE) {
		if (frame->frametype != AST_FRAME_IMAGE)
			ast_log(LOG_WARNING, "Don't know what to do with frame type '%d'\n", frame->frametype);
		return 0;
	}
	if ((frame->subclass != AST_FORMAT_SLINEAR) &&
	    (frame->subclass != AST_FORMAT_ULAW) &&
	    (frame->subclass != AST_FORMAT_ALAW)) {
		ast_log(LOG_WARNING, "Cannot handle frames in %d format\n", frame->subclass);
		return -1;
	}
	if (p->dialing) {
		ast_debug(1, "Dropping frame since I'm still dialing on %s...\n",ast->name);
		return 0;
	}
	if (!p->owner) {
		ast_debug(1, "Dropping frame since there is no active owner on %s...\n",ast->name);
		return 0;
	}
	if (p->cidspill) {
		ast_debug(1, "Dropping frame since I've still got a callerid spill\n");
		return 0;
	}
	/* Return if it's not valid data */
	if (!frame->data || !frame->datalen)
		return 0;

	if (frame->subclass == AST_FORMAT_SLINEAR) {
		if (!p->subs[index].linear) {
			p->subs[index].linear = 1;
			res = dahdi_setlinear(p->subs[index].dfd, p->subs[index].linear);
			if (res)
				ast_log(LOG_WARNING, "Unable to set linear mode on channel %d\n", p->channel);
		}
		res = my_dahdi_write(p, (unsigned char *)frame->data, frame->datalen, index, 1);
	} else {
		/* x-law already */
		if (p->subs[index].linear) {
			p->subs[index].linear = 0;
			res = dahdi_setlinear(p->subs[index].dfd, p->subs[index].linear);
			if (res)
				ast_log(LOG_WARNING, "Unable to set companded mode on channel %d\n", p->channel);
		}
		res = my_dahdi_write(p, (unsigned char *)frame->data, frame->datalen, index, 0);
	}
	if (res < 0) {
		ast_log(LOG_WARNING, "write failed: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

static int dahdi_indicate(struct ast_channel *chan, int condition, const void *data, size_t datalen)
{
	struct dahdi_pvt *p = chan->tech_pvt;
	int res=-1;
	int index;
	int func = DAHDI_FLASH;
	ast_mutex_lock(&p->lock);
	index = dahdi_get_index(chan, p, 0);
	ast_debug(1, "Requested indication %d on channel %s\n", condition, chan->name);
	if (index == SUB_REAL) {
		switch (condition) {
		case AST_CONTROL_BUSY:
#ifdef HAVE_PRI
			if (p->priindication_oob && ((p->sig == SIG_PRI) || (p->sig == SIG_BRI) || (p->sig == SIG_BRI_PTMP))) {
				chan->hangupcause = AST_CAUSE_USER_BUSY;
				chan->_softhangup |= AST_SOFTHANGUP_DEV;
				res = 0;
			} else if (!p->progress &&
					((p->sig == SIG_PRI) || (p->sig == SIG_BRI) || (p->sig == SIG_BRI_PTMP))
					&& p->pri && !p->outgoing) {
				if (p->pri->pri) {
					if (!pri_grab(p, p->pri)) {
						pri_progress(p->pri->pri,p->call, PVT_TO_CHANNEL(p), 1);
						pri_rel(p->pri);
					}
					else
						ast_log(LOG_WARNING, "Unable to grab PRI on span %d\n", p->span);
				}
				p->progress = 1;
				res = tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_BUSY);
			} else
#endif
#ifdef HAVE_SS7
			if (p->priindication_oob && p->sig == SIG_SS7) {
				chan->hangupcause = AST_CAUSE_USER_BUSY;
				chan->_softhangup |= AST_SOFTHANGUP_DEV;
				res = 0;
			} else if (!p->progress && p->sig == SIG_SS7) {
				if (p->ss7->ss7) {
					ss7_grab(p, p->ss7);
					isup_rel(p->ss7->ss7, p->ss7call, AST_CAUSE_BUSY);
					ss7_rel(p->ss7);
				}
				res = 0;
			} else
#endif
				res = tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_BUSY);
			break;
		case AST_CONTROL_RINGING:
#ifdef HAVE_PRI
			if ((!p->alerting) && ((p->sig == SIG_PRI) || (p->sig == SIG_BRI) || (p->sig == SIG_BRI_PTMP))
					&& p->pri && !p->outgoing && (chan->_state != AST_STATE_UP)) {
				if (p->pri->pri) {
					if (!pri_grab(p, p->pri)) {
						pri_acknowledge(p->pri->pri,p->call, PVT_TO_CHANNEL(p), !p->digital);
						pri_rel(p->pri);
					}
					else
						ast_log(LOG_WARNING, "Unable to grab PRI on span %d\n", p->span);
				}
				p->alerting = 1;
			}

#endif
#ifdef HAVE_SS7
			if ((!p->alerting) && (p->sig == SIG_SS7) && p->ss7 && !p->outgoing && (chan->_state != AST_STATE_UP)) {
				if (p->ss7->ss7) {
					ss7_grab(p, p->ss7);

					if ((isup_far(p->ss7->ss7, p->ss7call)) != -1)
						p->rlt = 1;
					if (p->rlt != 1) /* No need to send CPG if call will be RELEASE */
						isup_cpg(p->ss7->ss7, p->ss7call, CPG_EVENT_ALERTING);
					p->alerting = 1;
					ss7_rel(p->ss7);
				}
			}
#endif

			res = tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_RINGTONE);

			if (chan->_state != AST_STATE_UP) {
				if ((chan->_state != AST_STATE_RING) ||
					((p->sig != SIG_FXSKS) &&
				 (p->sig != SIG_FXSLS) &&
				 (p->sig != SIG_FXSGS)))
				ast_setstate(chan, AST_STATE_RINGING);
			}
			break;
		case AST_CONTROL_PROCEEDING:
			ast_debug(1,"Received AST_CONTROL_PROCEEDING on %s\n",chan->name);
#ifdef HAVE_PRI
			if (!p->proceeding && ((p->sig == SIG_PRI) || (p->sig == SIG_BRI) || (p->sig == SIG_BRI_PTMP))
					&& p->pri && !p->outgoing) {
				if (p->pri->pri) {
					if (!pri_grab(p, p->pri)) {
						pri_proceeding(p->pri->pri,p->call, PVT_TO_CHANNEL(p), !p->digital);
						pri_rel(p->pri);
					}
					else
						ast_log(LOG_WARNING, "Unable to grab PRI on span %d\n", p->span);
				}
				p->proceeding = 1;
			}
#endif
#ifdef HAVE_SS7
			/* This IF sends the FAR for an answered ALEG call */
			if (chan->_state == AST_STATE_UP && (p->rlt != 1) && (p->sig == SIG_SS7)){
				if ((isup_far(p->ss7->ss7, p->ss7call)) != -1)
					p->rlt = 1;
			}

			if (!p->proceeding && p->sig == SIG_SS7 && p->ss7 && !p->outgoing) {

				if (p->ss7->ss7) {
					ss7_grab(p, p->ss7);
					isup_acm(p->ss7->ss7, p->ss7call);
					p->proceeding = 1;
					ss7_rel(p->ss7);

				}
			}
#endif
			/* don't continue in ast_indicate */
			res = 0;
			break;
		case AST_CONTROL_PROGRESS:
			ast_debug(1,"Received AST_CONTROL_PROGRESS on %s\n",chan->name);
#ifdef HAVE_PRI
			p->digital = 0;	/* Digital-only calls isn't allows any inband progress messages */
			if (!p->progress && ((p->sig == SIG_PRI) || (p->sig == SIG_BRI) || (p->sig == SIG_BRI_PTMP))
					&& p->pri && !p->outgoing) {
				if (p->pri->pri) {
					if (!pri_grab(p, p->pri)) {
						pri_progress(p->pri->pri,p->call, PVT_TO_CHANNEL(p), 1);
						pri_rel(p->pri);
					}
					else
						ast_log(LOG_WARNING, "Unable to grab PRI on span %d\n", p->span);
				}
				p->progress = 1;
			}
#endif
#ifdef HAVE_SS7
			if (!p->progress && p->sig==SIG_SS7 && p->ss7 && !p->outgoing) {
				if (p->ss7->ss7) {
					ss7_grab(p, p->ss7);
					isup_cpg(p->ss7->ss7, p->ss7call, CPG_EVENT_INBANDINFO);
					p->progress = 1;
					ss7_rel(p->ss7);
					/* enable echo canceler here on SS7 calls */
					if(!p->echocontrol_ind || !(p->ss7->flags & LINKSET_FLAG_USEECHOCONTROL)) {
						dahdi_enable_ec(p);
						dahdi_train_ec(p);
					}

				}
			}
#endif
			/* don't continue in ast_indicate */
			res = 0;
			break;
		case AST_CONTROL_CONGESTION:
			chan->hangupcause = AST_CAUSE_CONGESTION;
#ifdef HAVE_PRI
			if (p->priindication_oob && ((p->sig == SIG_PRI) || (p->sig == SIG_BRI) || (p->sig == SIG_BRI_PTMP))) {
				chan->hangupcause = AST_CAUSE_SWITCH_CONGESTION;
				chan->_softhangup |= AST_SOFTHANGUP_DEV;
				res = 0;
			} else if (!p->progress && ((p->sig == SIG_PRI) || (p->sig == SIG_BRI) || (p->sig == SIG_BRI_PTMP))
					&& p->pri && !p->outgoing) {
				if (p->pri) {
					if (!pri_grab(p, p->pri)) {
						pri_progress(p->pri->pri,p->call, PVT_TO_CHANNEL(p), 1);
						pri_rel(p->pri);
					} else
						ast_log(LOG_WARNING, "Unable to grab PRI on span %d\n", p->span);
				}
				p->progress = 1;
				res = tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_CONGESTION);
			} else
#endif
#ifdef HAVE_SS7
			if (p->priindication_oob && p->sig == SIG_SS7) {
				chan->hangupcause = AST_CAUSE_SWITCH_CONGESTION;
				chan->_softhangup |= AST_SOFTHANGUP_DEV;
				res = 0;
			} else if (!p->progress && p->sig == SIG_SS7) {
				if (p->ss7->ss7) {
					ss7_grab(p, p->ss7);
					isup_rel(p->ss7->ss7, p->ss7call, AST_CAUSE_SWITCH_CONGESTION);
					ss7_rel(p->ss7);
				}
				res = 0;
			} else
#endif
				res = tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_CONGESTION);
			break;
		case AST_CONTROL_HOLD:
#ifdef HAVE_PRI
			if (p->pri && !strcasecmp(p->mohinterpret, "passthrough")) {
				if (!pri_grab(p, p->pri)) {
					res = pri_notify(p->pri->pri, p->call, p->prioffset, PRI_NOTIFY_REMOTE_HOLD);
					pri_rel(p->pri);
				} else
						ast_log(LOG_WARNING, "Unable to grab PRI on span %d\n", p->span);
			} else
#endif
				ast_moh_start(chan, data, p->mohinterpret);
#ifdef HAVE_SS7
			/* if (p->ss7) {
				if (!ss7_grab(p, p->ss7)) {
					if(p->ss7call)
						isup_sus(p->ss7->ss7, p->ss7call, 0);
					ss7_rel(p->ss7);
				} else
					ast_log(LOG_WARNING, "Unable to grab SS& on linkset %d\n", p->span);
			} it was just a mistake */
#endif
			break;
		case AST_CONTROL_UNHOLD:
#ifdef HAVE_PRI
			if (p->pri && !strcasecmp(p->mohinterpret, "passthrough")) {
				if (!pri_grab(p, p->pri)) {
					res = pri_notify(p->pri->pri, p->call, p->prioffset, PRI_NOTIFY_REMOTE_RETRIEVAL);
					pri_rel(p->pri);
				} else
						ast_log(LOG_WARNING, "Unable to grab PRI on span %d\n", p->span);
			} else
#endif
				ast_moh_stop(chan);
#ifdef HAVE_SS7
			/* if (p->ss7) {
				if (!ss7_grab(p, p->ss7)) {
					if(p->ss7call)
						isup_res(p->ss7->ss7, p->ss7call, 0);
					ss7_rel(p->ss7);
				} else
					ast_log(LOG_WARNING, "Unable to grab SS& on linkset %d\n", p->span);
			} it was just a mistake */
#endif
			break;
		case AST_CONTROL_RADIO_KEY:
			if (p->radio)
			    res =  dahdi_set_hook(p->subs[index].dfd, DAHDI_OFFHOOK);
			res = 0;
			break;
		case AST_CONTROL_RADIO_UNKEY:
			if (p->radio)
			    res =  dahdi_set_hook(p->subs[index].dfd, DAHDI_RINGOFF);
			res = 0;
			break;
		case AST_CONTROL_FLASH:
			/* flash hookswitch */
			if (ISTRUNK(p) && (p->sig != SIG_PRI)) {
				/* Clear out the dial buffer */
				p->dop.dialstr[0] = '\0';
				if ((ioctl(p->subs[SUB_REAL].dfd,DAHDI_HOOK,&func) == -1) && (errno != EINPROGRESS)) {
					ast_log(LOG_WARNING, "Unable to flash external trunk on channel %s: %s\n",
						chan->name, strerror(errno));
				} else
					res = 0;
			} else
				res = 0;
			break;
		case AST_CONTROL_SRCUPDATE:
			res = 0;
			break;
		case -1:
			res = tone_zone_play_tone(p->subs[index].dfd, -1);
			break;
		}
	} else
		res = 0;
	ast_mutex_unlock(&p->lock);
	return res;
}

static struct ast_channel *dahdi_new(struct dahdi_pvt *i, int state, int startpbx, int index, int law, int transfercapability)
{
	struct ast_channel *tmp;
	int deflaw;
	int res;
	int x,y;
	int features;
	struct ast_str *chan_name;
	struct ast_variable *v;
	struct dahdi_params ps;
	if (i->subs[index].owner) {
		ast_log(LOG_WARNING, "Channel %d already has a %s call\n", i->channel,subnames[index]);
		return NULL;
	}
	y = 1;
	chan_name = ast_str_alloca(32);
	do {
#ifdef HAVE_PRI
		if (i->bearer || (i->pri && (i->sig == SIG_FXSKS)))
			ast_str_set(&chan_name, 0, "%d:%d-%d", i->pri->trunkgroup, i->channel, y);
		else
#endif
		if (i->channel == CHAN_PSEUDO)
			ast_str_set(&chan_name, 0, "pseudo-%ld", ast_random());
		else
			ast_str_set(&chan_name, 0, "%d-%d", i->channel, y);
		for (x = 0; x < 3; x++) {
			if ((index != x) && i->subs[x].owner && !strcasecmp(chan_name->str, i->subs[x].owner->name + 6))
				break;
		}
		y++;
	} while (x < 3);
	tmp = ast_channel_alloc(0, state, i->cid_num, i->cid_name, i->accountcode, i->exten, i->context, i->amaflags, "DAHDI/%s", chan_name->str);
	if (!tmp)
		return NULL;
	tmp->tech = &dahdi_tech;
	memset(&ps, 0, sizeof(ps));
	ps.channo = i->channel;
	res = ioctl(i->subs[SUB_REAL].dfd, DAHDI_GET_PARAMS, &ps);
	if (res) {
		ast_log(LOG_WARNING, "Unable to get parameters, assuming MULAW: %s\n", strerror(errno));
		ps.curlaw = DAHDI_LAW_MULAW;
	}
	if (ps.curlaw == DAHDI_LAW_ALAW)
		deflaw = AST_FORMAT_ALAW;
	else
		deflaw = AST_FORMAT_ULAW;
	if (law) {
		if (law == DAHDI_LAW_ALAW)
			deflaw = AST_FORMAT_ALAW;
		else
			deflaw = AST_FORMAT_ULAW;
	}
	ast_channel_set_fd(tmp, 0, i->subs[index].dfd);
	tmp->nativeformats = deflaw;
	/* Start out assuming ulaw since it's smaller :) */
	tmp->rawreadformat = deflaw;
	tmp->readformat = deflaw;
	tmp->rawwriteformat = deflaw;
	tmp->writeformat = deflaw;
	i->subs[index].linear = 0;
	dahdi_setlinear(i->subs[index].dfd, i->subs[index].linear);
	features = 0;
	if (index == SUB_REAL) {
		if (i->busydetect && CANBUSYDETECT(i))
			features |= DSP_FEATURE_BUSY_DETECT;
		if ((i->callprogress & CALLPROGRESS_PROGRESS) && CANPROGRESSDETECT(i))
			features |= DSP_FEATURE_CALL_PROGRESS;
		if ((!i->outgoing && (i->callprogress & CALLPROGRESS_FAX_INCOMING)) ||
		    (i->outgoing && (i->callprogress & CALLPROGRESS_FAX_OUTGOING))) {
			features |= DSP_FEATURE_FAX_DETECT;
		}
		x = DAHDI_TONEDETECT_ON | DAHDI_TONEDETECT_MUTE;
		if (ioctl(i->subs[index].dfd, DAHDI_TONEDETECT, &x)) {
			i->hardwaredtmf = 0;
			features |= DSP_FEATURE_DTMF_DETECT;
		} else if (NEED_MFDETECT(i)) {
			i->hardwaredtmf = 1;
			features |= DSP_FEATURE_DTMF_DETECT;
		}
	}
	if (features) {
		if (i->dsp) {
			ast_debug(1, "Already have a dsp on %s?\n", tmp->name);
		} else {
			if (i->channel != CHAN_PSEUDO)
				i->dsp = ast_dsp_new();
			else
				i->dsp = NULL;
			if (i->dsp) {
				i->dsp_features = features;
#if defined(HAVE_PRI) || defined(HAVE_SS7)
				/* We cannot do progress detection until receives PROGRESS message */
				if (i->outgoing && ((i->sig == SIG_PRI) || (i->sig == SIG_BRI) || (i->sig == SIG_BRI_PTMP) || (i->sig == SIG_SS7))) {
					/* Remember requested DSP features, don't treat
					   talking as ANSWER */
					i->dsp_features = features & ~DSP_PROGRESS_TALK;
					features = 0;
				}
#endif
				ast_dsp_set_features(i->dsp, features);
				ast_dsp_digitmode(i->dsp, DSP_DIGITMODE_DTMF | i->dtmfrelax);
				if (!ast_strlen_zero(progzone))
					ast_dsp_set_call_progress_zone(i->dsp, progzone);
				if (i->busydetect && CANBUSYDETECT(i)) {
					ast_dsp_set_busy_count(i->dsp, i->busycount);
					ast_dsp_set_busy_pattern(i->dsp, i->busy_tonelength, i->busy_quietlength);
				}
			}
		}
	}

	if (state == AST_STATE_RING)
		tmp->rings = 1;
	tmp->tech_pvt = i;
	if ((i->sig == SIG_FXOKS) || (i->sig == SIG_FXOGS) || (i->sig == SIG_FXOLS)) {
		/* Only FXO signalled stuff can be picked up */
		tmp->callgroup = i->callgroup;
		tmp->pickupgroup = i->pickupgroup;
	}
	if (!ast_strlen_zero(i->language))
		ast_string_field_set(tmp, language, i->language);
	if (!i->owner)
		i->owner = tmp;
	if (!ast_strlen_zero(i->accountcode))
		ast_string_field_set(tmp, accountcode, i->accountcode);
	if (i->amaflags)
		tmp->amaflags = i->amaflags;
	i->subs[index].owner = tmp;
	ast_copy_string(tmp->context, i->context, sizeof(tmp->context));
	ast_string_field_set(tmp, call_forward, i->call_forward);
	/* If we've been told "no ADSI" then enforce it */
	if (!i->adsi)
		tmp->adsicpe = AST_ADSI_UNAVAILABLE;
	if (!ast_strlen_zero(i->exten))
		ast_copy_string(tmp->exten, i->exten, sizeof(tmp->exten));
	if (!ast_strlen_zero(i->rdnis))
		tmp->cid.cid_rdnis = ast_strdup(i->rdnis);
	if (!ast_strlen_zero(i->dnid))
		tmp->cid.cid_dnid = ast_strdup(i->dnid);

	/* Don't use ast_set_callerid() here because it will
	 * generate a needless NewCallerID event */
#ifdef PRI_ANI
	if (!ast_strlen_zero(i->cid_ani))
		tmp->cid.cid_ani = ast_strdup(i->cid_ani);
	else
		tmp->cid.cid_ani = ast_strdup(i->cid_num);
#else
	tmp->cid.cid_ani = ast_strdup(i->cid_num);
#endif
	tmp->cid.cid_pres = i->callingpres;
	tmp->cid.cid_ton = i->cid_ton;
	tmp->cid.cid_ani2 = i->cid_ani2;
#if defined(HAVE_PRI) || defined(HAVE_SS7)
	tmp->transfercapability = transfercapability;
	pbx_builtin_setvar_helper(tmp, "TRANSFERCAPABILITY", ast_transfercapability2str(transfercapability));
	if (transfercapability & AST_TRANS_CAP_DIGITAL)
		i->digital = 1;
	/* Assume calls are not idle calls unless we're told differently */
	i->isidlecall = 0;
	i->alreadyhungup = 0;
#endif
#ifdef HAVE_SS7
	i->do_hangup = SS7_HANGUP_SEND_REL;
#endif
	/* clear the fake event in case we posted one before we had ast_channel */
	i->fake_event = 0;
	/* Assure there is no confmute on this channel */
	dahdi_confmute(i, 0);
	/* Configure the new channel jb */
	ast_jb_configure(tmp, &global_jbconf);

	ast_device_state_changed_literal(tmp->name);

	for (v = i->vars ; v ; v = v->next)
                pbx_builtin_setvar_helper(tmp, v->name, v->value);

	if (startpbx) {
		if (ast_pbx_start(tmp)) {
			ast_log(LOG_WARNING, "Unable to start PBX on %s\n", tmp->name);
			ast_hangup(tmp);
			i->owner = NULL;
			return NULL;
		}
	}

	ast_module_ref(ast_module_info->self);
	return tmp;
}


static int my_getsigstr(struct ast_channel *chan, char *str, const char *term, int ms)
{
	char c;

	*str = 0; /* start with empty output buffer */
	for (;;)
	{
		/* Wait for the first digit (up to specified ms). */
		c = ast_waitfordigit(chan, ms);
		/* if timeout, hangup or error, return as such */
		if (c < 1)
			return c;
		*str++ = c;
		*str = 0;
		if (strchr(term, c))
			return 1;
	}
}

static int dahdi_wink(struct dahdi_pvt *p, int index)
{
	int j;
	dahdi_set_hook(p->subs[index].dfd, DAHDI_WINK);
	for (;;)
	{
		   /* set bits of interest */
		j = DAHDI_IOMUX_SIGEVENT;
		    /* wait for some happening */
		if (ioctl(p->subs[index].dfd,DAHDI_IOMUX,&j) == -1) return(-1);
		   /* exit loop if we have it */
		if (j & DAHDI_IOMUX_SIGEVENT) break;
	}
	  /* get the event info */
	if (ioctl(p->subs[index].dfd,DAHDI_GETEVENT,&j) == -1) return(-1);
	return 0;
}

/*! \brief enable or disable the chan_dahdi Do-Not-Disturb mode for a DAHDI channel
 * \param dahdichan "Physical" DAHDI channel (e.g: DAHDI/5)
 * \param on 1 to enable, 0 to disable
 *
 * chan_dahdi has a DND (Do Not Disturb) mode for each dahdichan (physical
 * dahdi channel). Use this to enable or disable it.
 *
 * \bug the use of the word "channel" for those dahdichans is really confusing.
 */
static void dahdi_dnd(struct dahdi_pvt *dahdichan, int on)
{
	/* Do not disturb */
	dahdichan->dnd = on;
	ast_verb(3, "%s DND on channel %d\n",
			on? "Enabled" : "Disabled",
			dahdichan->channel);
	manager_event(EVENT_FLAG_SYSTEM, "DNDState",
			"Channel: DAHDI/%d\r\n"
			"Status: %s\r\n", dahdichan->channel,
			on? "enabled" : "disabled");
}

static void *ss_thread(void *data)
{
	struct ast_channel *chan = data;
	struct dahdi_pvt *p = chan->tech_pvt;
	char exten[AST_MAX_EXTENSION] = "";
	char exten2[AST_MAX_EXTENSION] = "";
	unsigned char buf[256];
	char dtmfcid[300];
	char dtmfbuf[300];
	struct callerid_state *cs = NULL;
	char *name = NULL, *number = NULL;
	int distMatches;
	int curRingData[3];
	int receivedRingT;
	int counter1;
	int counter;
	int samples = 0;
	struct ast_smdi_md_message *smdi_msg = NULL;
	int flags;
	int i;
	int timeout;
	int getforward = 0;
	char *s1, *s2;
	int len = 0;
	int res;
	int index;

	ast_mutex_lock(&ss_thread_lock);
	ss_thread_count++;
	ast_mutex_unlock(&ss_thread_lock);
	/* in the bizarre case where the channel has become a zombie before we
	   even get started here, abort safely
	*/
	if (!p) {
		ast_log(LOG_WARNING, "Channel became a zombie before simple switch could be started (%s)\n", chan->name);
		ast_hangup(chan);
		goto quit;
	}
	ast_verb(3, "Starting simple switch on '%s'\n", chan->name);
	index = dahdi_get_index(chan, p, 1);
	if (index < 0) {
		ast_log(LOG_WARNING, "Huh?\n");
		ast_hangup(chan);
		goto quit;
	}
	if (p->dsp)
		ast_dsp_digitreset(p->dsp);
	switch (p->sig) {
#ifdef HAVE_PRI
	case SIG_PRI:
	case SIG_BRI:
	case SIG_BRI_PTMP:
		/* Now loop looking for an extension */
		ast_copy_string(exten, p->exten, sizeof(exten));
		len = strlen(exten);
		res = 0;
		while ((len < AST_MAX_EXTENSION-1) && ast_matchmore_extension(chan, chan->context, exten, 1, p->cid_num)) {
			if (len && !ast_ignore_pattern(chan->context, exten))
				tone_zone_play_tone(p->subs[index].dfd, -1);
			else
				tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_DIALTONE);
			if (ast_exists_extension(chan, chan->context, exten, 1, p->cid_num))
				timeout = matchdigittimeout;
			else
				timeout = gendigittimeout;
			res = ast_waitfordigit(chan, timeout);
			if (res < 0) {
				ast_debug(1, "waitfordigit returned < 0...\n");
				ast_hangup(chan);
				goto quit;
			} else if (res) {
				exten[len++] = res;
				exten[len] = '\0';
			} else
				break;
		}
		/* if no extension was received ('unspecified') on overlap call, use the 's' extension */
		if (ast_strlen_zero(exten)) {
			ast_verb(3, "Going to extension s|1 because of empty extension received on overlap call\n");
			exten[0] = 's';
			exten[1] = '\0';
		}
		tone_zone_play_tone(p->subs[index].dfd, -1);
		if (ast_exists_extension(chan, chan->context, exten, 1, p->cid_num)) {
			/* Start the real PBX */
			ast_copy_string(chan->exten, exten, sizeof(chan->exten));
			if (p->dsp) ast_dsp_digitreset(p->dsp);
			dahdi_enable_ec(p);
			ast_setstate(chan, AST_STATE_RING);
			res = ast_pbx_run(chan);
			if (res) {
				ast_log(LOG_WARNING, "PBX exited non-zero!\n");
			}
		} else {
			ast_debug(1, "No such possible extension '%s' in context '%s'\n", exten, chan->context);
			chan->hangupcause = AST_CAUSE_UNALLOCATED;
			ast_hangup(chan);
			p->exten[0] = '\0';
			/* Since we send release complete here, we won't get one */
			p->call = NULL;
		}
		goto quit;
		break;
#endif
	case SIG_FEATD:
	case SIG_FEATDMF:
	case SIG_FEATDMF_TA:
	case SIG_E911:
	case SIG_FGC_CAMAMF:
	case SIG_FEATB:
	case SIG_EMWINK:
	case SIG_SF_FEATD:
	case SIG_SF_FEATDMF:
	case SIG_SF_FEATB:
	case SIG_SFWINK:
		if (dahdi_wink(p, index))
			goto quit;
		/* Fall through */
	case SIG_EM:
	case SIG_EM_E1:
	case SIG_SF:
	case SIG_FGC_CAMA:
		res = tone_zone_play_tone(p->subs[index].dfd, -1);
		if (p->dsp)
			ast_dsp_digitreset(p->dsp);
		/* set digit mode appropriately */
		if (p->dsp) {
			if (NEED_MFDETECT(p))
				ast_dsp_digitmode(p->dsp, DSP_DIGITMODE_MF | p->dtmfrelax);
			else
				ast_dsp_digitmode(p->dsp, DSP_DIGITMODE_DTMF | p->dtmfrelax);
		}
		memset(dtmfbuf, 0, sizeof(dtmfbuf));
		/* Wait for the first digit only if immediate=no */
		if (!p->immediate)
			/* Wait for the first digit (up to 5 seconds). */
			res = ast_waitfordigit(chan, 5000);
		else
			res = 0;
		if (res > 0) {
			/* save first char */
			dtmfbuf[0] = res;
			switch (p->sig) {
			case SIG_FEATD:
			case SIG_SF_FEATD:
				res = my_getsigstr(chan, dtmfbuf + 1, "*", 3000);
				if (res > 0)
					res = my_getsigstr(chan, dtmfbuf + strlen(dtmfbuf), "*", 3000);
				if ((res < 1) && (p->dsp)) ast_dsp_digitreset(p->dsp);
				break;
			case SIG_FEATDMF_TA:
				res = my_getsigstr(chan, dtmfbuf + 1, "#", 3000);
				if ((res < 1) && (p->dsp)) ast_dsp_digitreset(p->dsp);
				if (dahdi_wink(p, index)) return NULL;
				dtmfbuf[0] = 0;
				/* Wait for the first digit (up to 5 seconds). */
				res = ast_waitfordigit(chan, 5000);
				if (res <= 0) break;
				dtmfbuf[0] = res;
				/* fall through intentionally */
			case SIG_FEATDMF:
			case SIG_E911:
			case SIG_FGC_CAMAMF:
			case SIG_SF_FEATDMF:
				res = my_getsigstr(chan, dtmfbuf + 1, "#", 3000);
				/* if international caca, do it again to get real ANO */
				if ((p->sig == SIG_FEATDMF) && (dtmfbuf[1] != '0') && (strlen(dtmfbuf) != 14))
				{
					if (dahdi_wink(p, index)) return NULL;
					dtmfbuf[0] = 0;
					/* Wait for the first digit (up to 5 seconds). */
					res = ast_waitfordigit(chan, 5000);
					if (res <= 0) break;
					dtmfbuf[0] = res;
					res = my_getsigstr(chan, dtmfbuf + 1, "#", 3000);
				}
				if (res > 0) {
					/* if E911, take off hook */
					if (p->sig == SIG_E911)
						dahdi_set_hook(p->subs[SUB_REAL].dfd, DAHDI_OFFHOOK);
					res = my_getsigstr(chan, dtmfbuf + strlen(dtmfbuf), "#", 3000);
				}
				if ((res < 1) && (p->dsp)) ast_dsp_digitreset(p->dsp);
				break;
			case SIG_FEATB:
			case SIG_SF_FEATB:
				res = my_getsigstr(chan, dtmfbuf + 1, "#", 3000);
				if ((res < 1) && (p->dsp)) ast_dsp_digitreset(p->dsp);
				break;
			case SIG_EMWINK:
				/* if we received a '*', we are actually receiving Feature Group D
				   dial syntax, so use that mode; otherwise, fall through to normal
				   mode
				*/
				if (res == '*') {
					res = my_getsigstr(chan, dtmfbuf + 1, "*", 3000);
					if (res > 0)
						res = my_getsigstr(chan, dtmfbuf + strlen(dtmfbuf), "*", 3000);
					if ((res < 1) && (p->dsp)) ast_dsp_digitreset(p->dsp);
					break;
				}
			default:
				/* If we got the first digit, get the rest */
				len = 1;
				dtmfbuf[len] = '\0';
				while ((len < AST_MAX_EXTENSION-1) && ast_matchmore_extension(chan, chan->context, dtmfbuf, 1, p->cid_num)) {
					if (ast_exists_extension(chan, chan->context, dtmfbuf, 1, p->cid_num)) {
						timeout = matchdigittimeout;
					} else {
						timeout = gendigittimeout;
					}
					res = ast_waitfordigit(chan, timeout);
					if (res < 0) {
						ast_debug(1, "waitfordigit returned < 0...\n");
						ast_hangup(chan);
						goto quit;
					} else if (res) {
						dtmfbuf[len++] = res;
						dtmfbuf[len] = '\0';
					} else {
						break;
					}
				}
				break;
			}
		}
		if (res == -1) {
			ast_log(LOG_WARNING, "getdtmf on channel %d: %s\n", p->channel, strerror(errno));
			ast_hangup(chan);
			goto quit;
		} else if (res < 0) {
			ast_debug(1, "Got hung up before digits finished\n");
			ast_hangup(chan);
			goto quit;
		}

		if (p->sig == SIG_FGC_CAMA) {
			char anibuf[100];

			if (ast_safe_sleep(chan,1000) == -1) {
	                        ast_hangup(chan);
	                        goto quit;
			}
                        dahdi_set_hook(p->subs[SUB_REAL].dfd, DAHDI_OFFHOOK);
                        ast_dsp_digitmode(p->dsp, DSP_DIGITMODE_MF | p->dtmfrelax);
                        res = my_getsigstr(chan, anibuf, "#", 10000);
                        if ((res > 0) && (strlen(anibuf) > 2)) {
				if (anibuf[strlen(anibuf) - 1] == '#')
					anibuf[strlen(anibuf) - 1] = 0;
				ast_set_callerid(chan, anibuf + 2, NULL, anibuf + 2);
			}
                        ast_dsp_digitmode(p->dsp, DSP_DIGITMODE_DTMF | p->dtmfrelax);
		}

		ast_copy_string(exten, dtmfbuf, sizeof(exten));
		if (ast_strlen_zero(exten))
			ast_copy_string(exten, "s", sizeof(exten));
		if (p->sig == SIG_FEATD || p->sig == SIG_EMWINK) {
			/* Look for Feature Group D on all E&M Wink and Feature Group D trunks */
			if (exten[0] == '*') {
				char *stringp=NULL;
				ast_copy_string(exten2, exten, sizeof(exten2));
				/* Parse out extension and callerid */
				stringp=exten2 +1;
				s1 = strsep(&stringp, "*");
				s2 = strsep(&stringp, "*");
				if (s2) {
					if (!ast_strlen_zero(p->cid_num))
						ast_set_callerid(chan, p->cid_num, NULL, p->cid_num);
					else
						ast_set_callerid(chan, s1, NULL, s1);
					ast_copy_string(exten, s2, sizeof(exten));
				} else
					ast_copy_string(exten, s1, sizeof(exten));
			} else if (p->sig == SIG_FEATD)
				ast_log(LOG_WARNING, "Got a non-Feature Group D input on channel %d.  Assuming E&M Wink instead\n", p->channel);
		}
		if ((p->sig == SIG_FEATDMF) || (p->sig == SIG_FEATDMF_TA)) {
			if (exten[0] == '*') {
				char *stringp=NULL;
				ast_copy_string(exten2, exten, sizeof(exten2));
				/* Parse out extension and callerid */
				stringp=exten2 +1;
				s1 = strsep(&stringp, "#");
				s2 = strsep(&stringp, "#");
				if (s2) {
					if (!ast_strlen_zero(p->cid_num))
						ast_set_callerid(chan, p->cid_num, NULL, p->cid_num);
					else
						if (*(s1 + 2))
							ast_set_callerid(chan, s1 + 2, NULL, s1 + 2);
					ast_copy_string(exten, s2 + 1, sizeof(exten));
				} else
					ast_copy_string(exten, s1 + 2, sizeof(exten));
			} else
				ast_log(LOG_WARNING, "Got a non-Feature Group D input on channel %d.  Assuming E&M Wink instead\n", p->channel);
		}
		if ((p->sig == SIG_E911) || (p->sig == SIG_FGC_CAMAMF)) {
			if (exten[0] == '*') {
				char *stringp=NULL;
				ast_copy_string(exten2, exten, sizeof(exten2));
				/* Parse out extension and callerid */
				stringp=exten2 +1;
				s1 = strsep(&stringp, "#");
				s2 = strsep(&stringp, "#");
				if (s2 && (*(s2 + 1) == '0')) {
					if (*(s2 + 2))
						ast_set_callerid(chan, s2 + 2, NULL, s2 + 2);
				}
				if (s1)	ast_copy_string(exten, s1, sizeof(exten));
				else ast_copy_string(exten, "911", sizeof(exten));
			} else
				ast_log(LOG_WARNING, "Got a non-E911/FGC CAMA input on channel %d.  Assuming E&M Wink instead\n", p->channel);
		}
		if (p->sig == SIG_FEATB) {
			if (exten[0] == '*') {
				char *stringp=NULL;
				ast_copy_string(exten2, exten, sizeof(exten2));
				/* Parse out extension and callerid */
				stringp=exten2 +1;
				s1 = strsep(&stringp, "#");
				ast_copy_string(exten, exten2 + 1, sizeof(exten));
			} else
				ast_log(LOG_WARNING, "Got a non-Feature Group B input on channel %d.  Assuming E&M Wink instead\n", p->channel);
		}
		if ((p->sig == SIG_FEATDMF) || (p->sig == SIG_FEATDMF_TA)) {
			dahdi_wink(p, index);
                        /* some switches require a minimum guard time between
                           the last FGD wink and something that answers
                           immediately. This ensures it */
                        if (ast_safe_sleep(chan,100)) goto quit;
		}
		dahdi_enable_ec(p);
		if (NEED_MFDETECT(p)) {
			if (p->dsp) {
				if (!p->hardwaredtmf)
					ast_dsp_digitmode(p->dsp, DSP_DIGITMODE_DTMF | p->dtmfrelax);
				else {
					ast_dsp_free(p->dsp);
					p->dsp = NULL;
				}
			}
		}

		if (ast_exists_extension(chan, chan->context, exten, 1, chan->cid.cid_num)) {
			ast_copy_string(chan->exten, exten, sizeof(chan->exten));
			if (p->dsp) ast_dsp_digitreset(p->dsp);
			res = ast_pbx_run(chan);
			if (res) {
				ast_log(LOG_WARNING, "PBX exited non-zero\n");
				res = tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_CONGESTION);
			}
			goto quit;
		} else {
			ast_verb(2, "Unknown extension '%s' in context '%s' requested\n", exten, chan->context);
			sleep(2);
			res = tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_INFO);
			if (res < 0)
				ast_log(LOG_WARNING, "Unable to start special tone on %d\n", p->channel);
			else
				sleep(1);
			res = ast_streamfile(chan, "ss-noservice", chan->language);
			if (res >= 0)
				ast_waitstream(chan, "");
			res = tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_CONGESTION);
			ast_hangup(chan);
			goto quit;
		}
		break;
	case SIG_FXOLS:
	case SIG_FXOGS:
	case SIG_FXOKS:
		/* Read the first digit */
		timeout = firstdigittimeout;
		/* If starting a threeway call, never timeout on the first digit so someone
		   can use flash-hook as a "hold" feature */
		if (p->subs[SUB_THREEWAY].owner)
			timeout = 999999;
		while (len < AST_MAX_EXTENSION-1) {
			/* Read digit unless it's supposed to be immediate, in which case the
			   only answer is 's' */
			if (p->immediate)
				res = 's';
			else
				res = ast_waitfordigit(chan, timeout);
			timeout = 0;
			if (res < 0) {
				ast_debug(1, "waitfordigit returned < 0...\n");
				res = tone_zone_play_tone(p->subs[index].dfd, -1);
				ast_hangup(chan);
				goto quit;
			} else if (res)  {
				ast_debug(1,"waitfordigit returned '%c' (%d), timeout = %d\n", res, res, timeout);
				exten[len++]=res;
				exten[len] = '\0';
			}
			if (!ast_ignore_pattern(chan->context, exten))
				tone_zone_play_tone(p->subs[index].dfd, -1);
			else
				tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_DIALTONE);
			if (ast_exists_extension(chan, chan->context, exten, 1, p->cid_num) && strcmp(exten, ast_parking_ext())) {
				if (!res || !ast_matchmore_extension(chan, chan->context, exten, 1, p->cid_num)) {
					if (getforward) {
						/* Record this as the forwarding extension */
						ast_copy_string(p->call_forward, exten, sizeof(p->call_forward));
						ast_verb(3, "Setting call forward to '%s' on channel %d\n", p->call_forward, p->channel);
						res = tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_DIALRECALL);
						if (res)
							break;
						usleep(500000);
						res = tone_zone_play_tone(p->subs[index].dfd, -1);
						sleep(1);
						memset(exten, 0, sizeof(exten));
						res = tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_DIALTONE);
						len = 0;
						getforward = 0;
					} else  {
						res = tone_zone_play_tone(p->subs[index].dfd, -1);
						ast_copy_string(chan->exten, exten, sizeof(chan->exten));
						if (!ast_strlen_zero(p->cid_num)) {
							if (!p->hidecallerid)
								ast_set_callerid(chan, p->cid_num, NULL, p->cid_num);
							else
								ast_set_callerid(chan, NULL, NULL, p->cid_num);
						}
						if (!ast_strlen_zero(p->cid_name)) {
							if (!p->hidecallerid)
								ast_set_callerid(chan, NULL, p->cid_name, NULL);
						}
						ast_setstate(chan, AST_STATE_RING);
						dahdi_enable_ec(p);
						res = ast_pbx_run(chan);
						if (res) {
							ast_log(LOG_WARNING, "PBX exited non-zero\n");
							res = tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_CONGESTION);
						}
						goto quit;
					}
				} else {
					/* It's a match, but they just typed a digit, and there is an ambiguous match,
					   so just set the timeout to matchdigittimeout and wait some more */
					timeout = matchdigittimeout;
				}
			} else if (res == 0) {
				ast_debug(1, "not enough digits (and no ambiguous match)...\n");
				res = tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_CONGESTION);
				dahdi_wait_event(p->subs[index].dfd);
				ast_hangup(chan);
				goto quit;
			} else if (p->callwaiting && !strcmp(exten, "*70")) {
				ast_verb(3, "Disabling call waiting on %s\n", chan->name);
				/* Disable call waiting if enabled */
				p->callwaiting = 0;
				res = tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_DIALRECALL);
				if (res) {
					ast_log(LOG_WARNING, "Unable to do dial recall on channel %s: %s\n",
						chan->name, strerror(errno));
				}
				len = 0;
				ioctl(p->subs[index].dfd,DAHDI_CONFDIAG,&len);
				memset(exten, 0, sizeof(exten));
				timeout = firstdigittimeout;

			} else if (!strcmp(exten,ast_pickup_ext())) {
				/* Scan all channels and see if there are any
				 * ringing channels that have call groups
				 * that equal this channels pickup group
				 */
			  	if (index == SUB_REAL) {
					/* Switch us from Third call to Call Wait */
				  	if (p->subs[SUB_THREEWAY].owner) {
						/* If you make a threeway call and the *8# a call, it should actually
						   look like a callwait */
						alloc_sub(p, SUB_CALLWAIT);
					  	swap_subs(p, SUB_CALLWAIT, SUB_THREEWAY);
						unalloc_sub(p, SUB_THREEWAY);
					}
					dahdi_enable_ec(p);
					if (ast_pickup_call(chan)) {
						ast_debug(1, "No call pickup possible...\n");
						res = tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_CONGESTION);
						dahdi_wait_event(p->subs[index].dfd);
					}
					ast_hangup(chan);
					goto quit;
				} else {
					ast_log(LOG_WARNING, "Huh?  Got *8# on call not on real\n");
					ast_hangup(chan);
					goto quit;
				}

			} else if (!p->hidecallerid && !strcmp(exten, "*67")) {
				ast_verb(3, "Disabling Caller*ID on %s\n", chan->name);
				/* Disable Caller*ID if enabled */
				p->hidecallerid = 1;
				if (chan->cid.cid_num)
					ast_free(chan->cid.cid_num);
				chan->cid.cid_num = NULL;
				if (chan->cid.cid_name)
					ast_free(chan->cid.cid_name);
				chan->cid.cid_name = NULL;
				res = tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_DIALRECALL);
				if (res) {
					ast_log(LOG_WARNING, "Unable to do dial recall on channel %s: %s\n",
						chan->name, strerror(errno));
				}
				len = 0;
				memset(exten, 0, sizeof(exten));
				timeout = firstdigittimeout;
			} else if (p->callreturn && !strcmp(exten, "*69")) {
				res = 0;
				if (!ast_strlen_zero(p->lastcid_num)) {
					res = ast_say_digit_str(chan, p->lastcid_num, "", chan->language);
				}
				if (!res)
					res = tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_DIALRECALL);
				break;
			} else if (!strcmp(exten, "*78")) {
				dahdi_dnd(p, 1);
				/* Do not disturb */
				res = tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_DIALRECALL);
				getforward = 0;
				memset(exten, 0, sizeof(exten));
				len = 0;
			} else if (!strcmp(exten, "*79")) {
				dahdi_dnd(p, 0);
				/* Do not disturb */
				res = tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_DIALRECALL);
				getforward = 0;
				memset(exten, 0, sizeof(exten));
				len = 0;
			} else if (p->cancallforward && !strcmp(exten, "*72")) {
				res = tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_DIALRECALL);
				getforward = 1;
				memset(exten, 0, sizeof(exten));
				len = 0;
			} else if (p->cancallforward && !strcmp(exten, "*73")) {
				ast_verb(3, "Cancelling call forwarding on channel %d\n", p->channel);
				res = tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_DIALRECALL);
				memset(p->call_forward, 0, sizeof(p->call_forward));
				getforward = 0;
				memset(exten, 0, sizeof(exten));
				len = 0;
			} else if ((p->transfer || p->canpark) && !strcmp(exten, ast_parking_ext()) &&
						p->subs[SUB_THREEWAY].owner &&
						ast_bridged_channel(p->subs[SUB_THREEWAY].owner)) {
				/* This is a three way call, the main call being a real channel,
					and we're parking the first call. */
				ast_masq_park_call(ast_bridged_channel(p->subs[SUB_THREEWAY].owner), chan, 0, NULL);
				ast_verb(3, "Parking call to '%s'\n", chan->name);
				break;
			} else if (!ast_strlen_zero(p->lastcid_num) && !strcmp(exten, "*60")) {
				ast_verb(3, "Blacklisting number %s\n", p->lastcid_num);
				res = ast_db_put("blacklist", p->lastcid_num, "1");
				if (!res) {
					res = tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_DIALRECALL);
					memset(exten, 0, sizeof(exten));
					len = 0;
				}
			} else if (p->hidecallerid && !strcmp(exten, "*82")) {
				ast_verb(3, "Enabling Caller*ID on %s\n", chan->name);
				/* Enable Caller*ID if enabled */
				p->hidecallerid = 0;
				if (chan->cid.cid_num)
					ast_free(chan->cid.cid_num);
				chan->cid.cid_num = NULL;
				if (chan->cid.cid_name)
					ast_free(chan->cid.cid_name);
				chan->cid.cid_name = NULL;
				ast_set_callerid(chan, p->cid_num, p->cid_name, NULL);
				res = tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_DIALRECALL);
				if (res) {
					ast_log(LOG_WARNING, "Unable to do dial recall on channel %s: %s\n",
						chan->name, strerror(errno));
				}
				len = 0;
				memset(exten, 0, sizeof(exten));
				timeout = firstdigittimeout;
			} else if (!strcmp(exten, "*0")) {
				struct ast_channel *nbridge =
					p->subs[SUB_THREEWAY].owner;
				struct dahdi_pvt *pbridge = NULL;
				  /* set up the private struct of the bridged one, if any */
				if (nbridge && ast_bridged_channel(nbridge))
					pbridge = ast_bridged_channel(nbridge)->tech_pvt;
				if (nbridge && pbridge &&
				    (nbridge->tech == &dahdi_tech) &&
				    (ast_bridged_channel(nbridge)->tech == &dahdi_tech) &&
				    ISTRUNK(pbridge)) {
					int func = DAHDI_FLASH;
					/* Clear out the dial buffer */
					p->dop.dialstr[0] = '\0';
					/* flash hookswitch */
					if ((ioctl(pbridge->subs[SUB_REAL].dfd,DAHDI_HOOK,&func) == -1) && (errno != EINPROGRESS)) {
						ast_log(LOG_WARNING, "Unable to flash external trunk on channel %s: %s\n",
							nbridge->name, strerror(errno));
					}
					swap_subs(p, SUB_REAL, SUB_THREEWAY);
					unalloc_sub(p, SUB_THREEWAY);
					p->owner = p->subs[SUB_REAL].owner;
					if (ast_bridged_channel(p->subs[SUB_REAL].owner))
						ast_queue_control(p->subs[SUB_REAL].owner, AST_CONTROL_UNHOLD);
					ast_hangup(chan);
					goto quit;
				} else {
					tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_CONGESTION);
					dahdi_wait_event(p->subs[index].dfd);
					tone_zone_play_tone(p->subs[index].dfd, -1);
					swap_subs(p, SUB_REAL, SUB_THREEWAY);
					unalloc_sub(p, SUB_THREEWAY);
					p->owner = p->subs[SUB_REAL].owner;
					ast_hangup(chan);
					goto quit;
				}
			} else if (!ast_canmatch_extension(chan, chan->context, exten, 1, chan->cid.cid_num) &&
							((exten[0] != '*') || (strlen(exten) > 2))) {
				ast_debug(1, "Can't match %s from '%s' in context %s\n", exten, chan->cid.cid_num ? chan->cid.cid_num : "<Unknown Caller>", chan->context);
				break;
			}
			if (!timeout)
				timeout = gendigittimeout;
			if (len && !ast_ignore_pattern(chan->context, exten))
				tone_zone_play_tone(p->subs[index].dfd, -1);
		}
		break;
	case SIG_FXSLS:
	case SIG_FXSGS:
	case SIG_FXSKS:
#ifdef HAVE_PRI
		if (p->pri) {
			/* This is a GR-303 trunk actually.  Wait for the first ring... */
			struct ast_frame *f;
			int res;
			time_t start;

			time(&start);
			ast_setstate(chan, AST_STATE_RING);
			while (time(NULL) < start + 3) {
				res = ast_waitfor(chan, 1000);
				if (res) {
					f = ast_read(chan);
					if (!f) {
						ast_log(LOG_WARNING, "Whoa, hangup while waiting for first ring!\n");
						ast_hangup(chan);
						goto quit;
					} else if ((f->frametype == AST_FRAME_CONTROL) && (f->subclass == AST_CONTROL_RING)) {
						res = 1;
					} else
						res = 0;
					ast_frfree(f);
					if (res) {
						ast_debug(1, "Got ring!\n");
						res = 0;
						break;
					}
				}
			}
		}
#endif
		/* check for SMDI messages */
		if (p->use_smdi && p->smdi_iface) {
			smdi_msg = ast_smdi_md_message_wait(p->smdi_iface, SMDI_MD_WAIT_TIMEOUT);

			if (smdi_msg != NULL) {
				ast_copy_string(chan->exten, smdi_msg->fwd_st, sizeof(chan->exten));

				if (smdi_msg->type == 'B')
					pbx_builtin_setvar_helper(chan, "_SMDI_VM_TYPE", "b");
				else if (smdi_msg->type == 'N')
					pbx_builtin_setvar_helper(chan, "_SMDI_VM_TYPE", "u");

				ast_debug(1, "Recieved SMDI message on %s\n", chan->name);
			} else {
				ast_log(LOG_WARNING, "SMDI enabled but no SMDI message present\n");
			}
		}

		if (p->use_callerid && (p->cid_signalling == CID_SIG_SMDI && smdi_msg)) {
	     		number = smdi_msg->calling_st;

		/* If we want caller id, we're in a prering state due to a polarity reversal
		 * and we're set to use a polarity reversal to trigger the start of caller id,
		 * grab the caller id and wait for ringing to start... */
		} else if (p->use_callerid && (chan->_state == AST_STATE_PRERING && (p->cid_start == CID_START_POLARITY || p->cid_start == CID_START_POLARITY_IN))) {
			/* If set to use DTMF CID signalling, listen for DTMF */
			if (p->cid_signalling == CID_SIG_DTMF) {
				int i = 0;
				cs = NULL;
				ast_debug(1, "Receiving DTMF cid on "
					"channel %s\n", chan->name);
				dahdi_setlinear(p->subs[index].dfd, 0);
				res = 2000;
				for (;;) {
					struct ast_frame *f;
					res = ast_waitfor(chan, res);
					if (res <= 0) {
						ast_log(LOG_WARNING, "DTMFCID timed out waiting for ring. "
							"Exiting simple switch\n");
						ast_hangup(chan);
						goto quit;
					}
					f = ast_read(chan);
					if (!f)
						break;
					if (f->frametype == AST_FRAME_DTMF) {
						dtmfbuf[i++] = f->subclass;
						ast_debug(1, "CID got digit '%c'\n", f->subclass);
						res = 2000;
					}
					ast_frfree(f);
					if (chan->_state == AST_STATE_RING ||
					    chan->_state == AST_STATE_RINGING)
						break; /* Got ring */
				}
				dtmfbuf[i] = '\0';
				dahdi_setlinear(p->subs[index].dfd, p->subs[index].linear);
				/* Got cid and ring. */
				ast_debug(1, "CID got string '%s'\n", dtmfbuf);
				callerid_get_dtmf(dtmfbuf, dtmfcid, &flags);
				ast_debug(1, "CID is '%s', flags %d\n",
					dtmfcid, flags);
				/* If first byte is NULL, we have no cid */
				if (!ast_strlen_zero(dtmfcid))
					number = dtmfcid;
				else
					number = NULL;
			/* If set to use V23 Signalling, launch our FSK gubbins and listen for it */
			} else if ((p->cid_signalling == CID_SIG_V23) || (p->cid_signalling == CID_SIG_V23_JP)) {
				cs = callerid_new(p->cid_signalling);
				if (cs) {
					samples = 0;
#if 1
					bump_gains(p);
#endif
					/* Take out of linear mode for Caller*ID processing */
					dahdi_setlinear(p->subs[index].dfd, 0);

					/* First we wait and listen for the Caller*ID */
					for (;;) {
						i = DAHDI_IOMUX_READ | DAHDI_IOMUX_SIGEVENT;
						if ((res = ioctl(p->subs[index].dfd, DAHDI_IOMUX, &i)))	{
							ast_log(LOG_WARNING, "I/O MUX failed: %s\n", strerror(errno));
							callerid_free(cs);
							ast_hangup(chan);
							goto quit;
						}
						if (i & DAHDI_IOMUX_SIGEVENT) {
							res = dahdi_get_event(p->subs[index].dfd);
							ast_log(LOG_NOTICE, "Got event %d (%s)...\n", res, event2str(res));

							if (p->cid_signalling == CID_SIG_V23_JP) {
								if (res == DAHDI_EVENT_RINGBEGIN) {
									res = dahdi_set_hook(p->subs[SUB_REAL].dfd, DAHDI_OFFHOOK);
									usleep(1);
								}
							} else {
								res = 0;
								break;
							}
						} else if (i & DAHDI_IOMUX_READ) {
							res = read(p->subs[index].dfd, buf, sizeof(buf));
							if (res < 0) {
								if (errno != ELAST) {
									ast_log(LOG_WARNING, "read returned error: %s\n", strerror(errno));
									callerid_free(cs);
									ast_hangup(chan);
									goto quit;
								}
								break;
							}
							samples += res;

							if  (p->cid_signalling == CID_SIG_V23_JP) {
								res = callerid_feed_jp(cs, buf, res, AST_LAW(p));
							} else {
								res = callerid_feed(cs, buf, res, AST_LAW(p));
							}

							if (res < 0) {
								ast_log(LOG_WARNING, "CallerID feed failed on channel '%s'\n", chan->name);
								break;
							} else if (res)
								break;
							else if (samples > (8000 * 10))
								break;
						}
					}
					if (res == 1) {
						callerid_get(cs, &name, &number, &flags);
						ast_log(LOG_NOTICE, "CallerID number: %s, name: %s, flags=%d\n", number, name, flags);
					}

					if (p->cid_signalling == CID_SIG_V23_JP) {
						res = dahdi_set_hook(p->subs[SUB_REAL].dfd, DAHDI_ONHOOK);
						usleep(1);
						res = 4000;
					} else {

						/* Finished with Caller*ID, now wait for a ring to make sure there really is a call coming */
						res = 2000;
					}

					for (;;) {
						struct ast_frame *f;
						res = ast_waitfor(chan, res);
						if (res <= 0) {
							ast_log(LOG_WARNING, "CID timed out waiting for ring. "
								"Exiting simple switch\n");
							ast_hangup(chan);
							goto quit;
						}
						if (!(f = ast_read(chan))) {
							ast_log(LOG_WARNING, "Hangup received waiting for ring. Exiting simple switch\n");
							ast_hangup(chan);
							goto quit;
						}
						ast_frfree(f);
						if (chan->_state == AST_STATE_RING ||
						    chan->_state == AST_STATE_RINGING)
							break; /* Got ring */
					}

					/* We must have a ring by now, so, if configured, lets try to listen for
					 * distinctive ringing */
					if (p->usedistinctiveringdetection == 1) {
						len = 0;
						distMatches = 0;
						/* Clear the current ring data array so we dont have old data in it. */
						for (receivedRingT = 0; receivedRingT < ARRAY_LEN(curRingData); receivedRingT++)
							curRingData[receivedRingT] = 0;
						receivedRingT = 0;
						counter = 0;
						counter1 = 0;
						/* Check to see if context is what it should be, if not set to be. */
						if (strcmp(p->context,p->defcontext) != 0) {
							ast_copy_string(p->context, p->defcontext, sizeof(p->context));
							ast_copy_string(chan->context,p->defcontext,sizeof(chan->context));
						}

						for (;;) {
							i = DAHDI_IOMUX_READ | DAHDI_IOMUX_SIGEVENT;
							if ((res = ioctl(p->subs[index].dfd, DAHDI_IOMUX, &i)))	{
								ast_log(LOG_WARNING, "I/O MUX failed: %s\n", strerror(errno));
								callerid_free(cs);
								ast_hangup(chan);
								goto quit;
							}
							if (i & DAHDI_IOMUX_SIGEVENT) {
								res = dahdi_get_event(p->subs[index].dfd);
								ast_log(LOG_NOTICE, "Got event %d (%s)...\n", res, event2str(res));
								res = 0;
								/* Let us detect distinctive ring */

								curRingData[receivedRingT] = p->ringt;

								if (p->ringt < p->ringt_base/2)
									break;
								/* Increment the ringT counter so we can match it against
								   values in chan_dahdi.conf for distinctive ring */
								if (++receivedRingT == ARRAY_LEN(curRingData))
									break;
							} else if (i & DAHDI_IOMUX_READ) {
								res = read(p->subs[index].dfd, buf, sizeof(buf));
								if (res < 0) {
									if (errno != ELAST) {
										ast_log(LOG_WARNING, "read returned error: %s\n", strerror(errno));
										callerid_free(cs);
										ast_hangup(chan);
										goto quit;
									}
									break;
								}
								if (p->ringt)
									p->ringt--;
								if (p->ringt == 1) {
									res = -1;
									break;
								}
							}
						}
							/* this only shows up if you have n of the dring patterns filled in */
						ast_verb(3, "Detected ring pattern: %d,%d,%d\n",curRingData[0],curRingData[1],curRingData[2]);
						for (counter = 0; counter < 3; counter++) {
							/* Check to see if the rings we received match any of the ones in chan_dahdi.conf for this
							channel */
							distMatches = 0;
							for (counter1 = 0; counter1 < 3; counter1++) {
								ast_verb(3, "Ring pattern check range: %d\n", p->drings.ringnum[counter].range);
								if (p->drings.ringnum[counter].ring[counter1] == -1) {
									ast_verb(3, "Pattern ignore (-1) detected, so matching pattern %d regardless.\n",
									curRingData[counter1]);
									distMatches++;
								}
								else if (curRingData[counter1] <= (p->drings.ringnum[counter].ring[counter1] + p->drings.ringnum[counter].range) &&
								    curRingData[counter1] >= (p->drings.ringnum[counter].ring[counter1] - p->drings.ringnum[counter].range)) {
									ast_verb(3, "Ring pattern matched in range: %d to %d\n",
									(p->drings.ringnum[counter].ring[counter1] - p->drings.ringnum[counter].range),
									(p->drings.ringnum[counter].ring[counter1] + p->drings.ringnum[counter].range));
									distMatches++;
								}
							}

							if (distMatches == 3) {
								/* The ring matches, set the context to whatever is for distinctive ring.. */
								ast_copy_string(p->context, p->drings.ringContext[counter].contextData, sizeof(p->context));
								ast_copy_string(chan->context, p->drings.ringContext[counter].contextData, sizeof(chan->context));
								ast_verb(3, "Distinctive Ring matched context %s\n",p->context);
								break;
							}
						}
					}
					/* Restore linear mode (if appropriate) for Caller*ID processing */
					dahdi_setlinear(p->subs[index].dfd, p->subs[index].linear);
#if 1
					restore_gains(p);
#endif
				} else
					ast_log(LOG_WARNING, "Unable to get caller ID space\n");
			} else {
				ast_log(LOG_WARNING, "Channel %s in prering "
					"state, but I have nothing to do. "
					"Terminating simple switch, should be "
					"restarted by the actual ring.\n",
					chan->name);
				ast_hangup(chan);
				goto quit;
			}
		} else if (p->use_callerid && p->cid_start == CID_START_RING) {
                        if (p->cid_signalling == CID_SIG_DTMF) {
                                int i = 0;
                                cs = NULL;
                                dahdi_setlinear(p->subs[index].dfd, 0);
                                res = 2000;
                                for (;;) {
                                        struct ast_frame *f;
                                        res = ast_waitfor(chan, res);
                                        if (res <= 0) {
                                                ast_log(LOG_WARNING, "DTMFCID timed out waiting for ring. "
                                                                "Exiting simple switch\n");
                                                ast_hangup(chan);
                                                return NULL;
                                        }
                                        f = ast_read(chan);
                                        if (f->frametype == AST_FRAME_DTMF) {
                                                dtmfbuf[i++] = f->subclass;
                                                ast_log(LOG_DEBUG, "CID got digit '%c'\n", f->subclass);
                                                res = 2000;
                                        }
                                        ast_frfree(f);

                                        if (p->ringt_base == p->ringt)
                                                break;

                                }
                                dtmfbuf[i] = '\0';
                                dahdi_setlinear(p->subs[index].dfd, p->subs[index].linear);
                                /* Got cid and ring. */
                                callerid_get_dtmf(dtmfbuf, dtmfcid, &flags);
                                ast_log(LOG_DEBUG, "CID is '%s', flags %d\n",
                                                dtmfcid, flags);
                                /* If first byte is NULL, we have no cid */
                                if (!ast_strlen_zero(dtmfcid))
                                        number = dtmfcid;
                                else
                                        number = NULL;
                                /* If set to use V23 Signalling, launch our FSK gubbins and listen for it */
                        } else {
			/* FSK Bell202 callerID */
			cs = callerid_new(p->cid_signalling);
			if (cs) {
#if 1
				bump_gains(p);
#endif
				samples = 0;
				len = 0;
				distMatches = 0;
				/* Clear the current ring data array so we dont have old data in it. */
				for (receivedRingT = 0; receivedRingT < ARRAY_LEN(curRingData); receivedRingT++)
					curRingData[receivedRingT] = 0;
				receivedRingT = 0;
				counter = 0;
				counter1 = 0;
				/* Check to see if context is what it should be, if not set to be. */
				if (strcmp(p->context,p->defcontext) != 0) {
					ast_copy_string(p->context, p->defcontext, sizeof(p->context));
					ast_copy_string(chan->context,p->defcontext,sizeof(chan->context));
				}

				/* Take out of linear mode for Caller*ID processing */
				dahdi_setlinear(p->subs[index].dfd, 0);
				for (;;) {
					i = DAHDI_IOMUX_READ | DAHDI_IOMUX_SIGEVENT;
					if ((res = ioctl(p->subs[index].dfd, DAHDI_IOMUX, &i)))	{
						ast_log(LOG_WARNING, "I/O MUX failed: %s\n", strerror(errno));
						callerid_free(cs);
						ast_hangup(chan);
						goto quit;
					}
					if (i & DAHDI_IOMUX_SIGEVENT) {
						res = dahdi_get_event(p->subs[index].dfd);
						ast_log(LOG_NOTICE, "Got event %d (%s)...\n", res, event2str(res));
						/* If we get a PR event, they hung up while processing calerid */
						if ( res == DAHDI_EVENT_POLARITY && p->hanguponpolarityswitch && p->polarity == POLARITY_REV) {
							ast_log(LOG_DEBUG, "Hanging up due to polarity reversal on channel %d while detecting callerid\n", p->channel);
							p->polarity = POLARITY_IDLE;
							callerid_free(cs);
							ast_hangup(chan);
							goto quit;
						}
						res = 0;
						/* Let us detect callerid when the telco uses distinctive ring */

						curRingData[receivedRingT] = p->ringt;

						if (p->ringt < p->ringt_base/2)
							break;
						/* Increment the ringT counter so we can match it against
						   values in chan_dahdi.conf for distinctive ring */
						if (++receivedRingT == ARRAY_LEN(curRingData))
							break;
					} else if (i & DAHDI_IOMUX_READ) {
						res = read(p->subs[index].dfd, buf, sizeof(buf));
						if (res < 0) {
							if (errno != ELAST) {
								ast_log(LOG_WARNING, "read returned error: %s\n", strerror(errno));
								callerid_free(cs);
								ast_hangup(chan);
								goto quit;
							}
							break;
						}
						if (p->ringt)
							p->ringt--;
						if (p->ringt == 1) {
							res = -1;
							break;
						}
						samples += res;
						res = callerid_feed(cs, buf, res, AST_LAW(p));
						if (res < 0) {
							ast_log(LOG_WARNING, "CallerID feed failed: %s\n", strerror(errno));
							break;
						} else if (res)
							break;
						else if (samples > (8000 * 10))
							break;
					}
				}
				if (res == 1) {
					callerid_get(cs, &name, &number, &flags);
					ast_debug(1, "CallerID number: %s, name: %s, flags=%d\n", number, name, flags);
				}
				if (distinctiveringaftercid == 1) {
					/* Clear the current ring data array so we dont have old data in it. */
					for (receivedRingT = 0; receivedRingT < 3; receivedRingT++) {
						curRingData[receivedRingT] = 0;
					}
					receivedRingT = 0;
					ast_verb(3, "Detecting post-CID distinctive ring\n");
					for (;;) {
						i = DAHDI_IOMUX_READ | DAHDI_IOMUX_SIGEVENT;
						if ((res = ioctl(p->subs[index].dfd, DAHDI_IOMUX, &i)))    {
							ast_log(LOG_WARNING, "I/O MUX failed: %s\n", strerror(errno));
							callerid_free(cs);
							ast_hangup(chan);
							goto quit;
						}
						if (i & DAHDI_IOMUX_SIGEVENT) {
							res = dahdi_get_event(p->subs[index].dfd);
							ast_log(LOG_NOTICE, "Got event %d (%s)...\n", res, event2str(res));
							res = 0;
							/* Let us detect callerid when the telco uses distinctive ring */

							curRingData[receivedRingT] = p->ringt;

							if (p->ringt < p->ringt_base/2)
								break;
							/* Increment the ringT counter so we can match it against
							   values in chan_dahdi.conf for distinctive ring */
							if (++receivedRingT == ARRAY_LEN(curRingData))
								break;
						} else if (i & DAHDI_IOMUX_READ) {
							res = read(p->subs[index].dfd, buf, sizeof(buf));
							if (res < 0) {
								if (errno != ELAST) {
									ast_log(LOG_WARNING, "read returned error: %s\n", strerror(errno));
									callerid_free(cs);
									ast_hangup(chan);
									goto quit;
								}
								break;
							}
						if (p->ringt)
							p->ringt--;
							if (p->ringt == 1) {
								res = -1;
								break;
							}
						}
					}
				}
				if (p->usedistinctiveringdetection == 1) {
						/* this only shows up if you have n of the dring patterns filled in */
					ast_verb(3, "Detected ring pattern: %d,%d,%d\n",curRingData[0],curRingData[1],curRingData[2]);

					for (counter = 0; counter < 3; counter++) {
						/* Check to see if the rings we received match any of the ones in chan_dahdi.conf for this
						channel */
							/* this only shows up if you have n of the dring patterns filled in */
						ast_verb(3, "Checking %d,%d,%d\n",
								p->drings.ringnum[counter].ring[0],
								p->drings.ringnum[counter].ring[1],
								p->drings.ringnum[counter].ring[2]);
						distMatches = 0;
						for (counter1 = 0; counter1 < 3; counter1++) {
							ast_verb(3, "Ring pattern check range: %d\n", p->drings.ringnum[counter].range);
							if (p->drings.ringnum[counter].ring[counter1] == -1) {
								ast_verb(3, "Pattern ignore (-1) detected, so matching pattern %d regardless.\n",
								curRingData[counter1]);
								distMatches++;
							}
							else if (curRingData[counter1] <= (p->drings.ringnum[counter].ring[counter1] + p->drings.ringnum[counter].range) &&
							    curRingData[counter1] >= (p->drings.ringnum[counter].ring[counter1] - p->drings.ringnum[counter].range)) {
								ast_verb(3, "Ring pattern matched in range: %d to %d\n",
								(p->drings.ringnum[counter].ring[counter1] - p->drings.ringnum[counter].range),
								(p->drings.ringnum[counter].ring[counter1] + p->drings.ringnum[counter].range));
								distMatches++;
							}
						}
						if (distMatches == 3) {
							/* The ring matches, set the context to whatever is for distinctive ring.. */
							ast_copy_string(p->context, p->drings.ringContext[counter].contextData, sizeof(p->context));
							ast_copy_string(chan->context, p->drings.ringContext[counter].contextData, sizeof(chan->context));
							ast_verb(3, "Distinctive Ring matched context %s\n",p->context);
							break;
						}
					}
				}
				/* Restore linear mode (if appropriate) for Caller*ID processing */
				dahdi_setlinear(p->subs[index].dfd, p->subs[index].linear);
#if 1
				restore_gains(p);
#endif
				if (res < 0) {
					ast_log(LOG_WARNING, "CallerID returned with error on channel '%s'\n", chan->name);
				}
			} else
				ast_log(LOG_WARNING, "Unable to get caller ID space\n");
		}
		}
		else
			cs = NULL;

		if (number)
			ast_shrink_phone_number(number);
		ast_set_callerid(chan, number, name, number);

		if (smdi_msg)
			ASTOBJ_UNREF(smdi_msg, ast_smdi_md_message_destroy);

		if (cs)
			callerid_free(cs);

		ast_setstate(chan, AST_STATE_RING);
		chan->rings = 1;
		p->ringt = p->ringt_base;
		res = ast_pbx_run(chan);
		if (res) {
			ast_hangup(chan);
			ast_log(LOG_WARNING, "PBX exited non-zero\n");
		}
		goto quit;
	default:
		ast_log(LOG_WARNING, "Don't know how to handle simple switch with signalling %s on channel %d\n", sig2str(p->sig), p->channel);
		res = tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_CONGESTION);
		if (res < 0)
				ast_log(LOG_WARNING, "Unable to play congestion tone on channel %d\n", p->channel);
	}
	res = tone_zone_play_tone(p->subs[index].dfd, DAHDI_TONE_CONGESTION);
	if (res < 0)
			ast_log(LOG_WARNING, "Unable to play congestion tone on channel %d\n", p->channel);
	ast_hangup(chan);
quit:
	ast_mutex_lock(&ss_thread_lock);
	ss_thread_count--;
	ast_cond_signal(&ss_thread_complete);
	ast_mutex_unlock(&ss_thread_lock);
	return NULL;
}

struct mwi_thread_data {
	struct dahdi_pvt *pvt;
	unsigned char buf[READ_SIZE];
	size_t len;
};

static int calc_energy(const unsigned char *buf, int len, int law)
{
	int x;
	int sum = 0;

	if (!len)
		return 0;

	for (x = 0; x < len; x++)
		sum += abs(law == AST_FORMAT_ULAW ? AST_MULAW(buf[x]) : AST_ALAW(buf[x]));

	return sum / len;
}

static void *mwi_thread(void *data)
{
	struct mwi_thread_data *mtd = data;
	struct callerid_state *cs;
	pthread_t threadid;
	int samples = 0;
	char *name, *number;
	int flags;
	int i, res;
	unsigned int spill_done = 0;
	int spill_result = -1;

	if (!(cs = callerid_new(mtd->pvt->cid_signalling))) {
		mtd->pvt->mwimonitoractive = 0;

		return NULL;
	}

	callerid_feed(cs, mtd->buf, mtd->len, AST_LAW(mtd->pvt));

	bump_gains(mtd->pvt);

	for (;;) {
		i = DAHDI_IOMUX_READ | DAHDI_IOMUX_SIGEVENT;
		if ((res = ioctl(mtd->pvt->subs[SUB_REAL].dfd, DAHDI_IOMUX, &i))) {
			ast_log(LOG_WARNING, "I/O MUX failed: %s\n", strerror(errno));
			goto quit;
		}

		if (i & DAHDI_IOMUX_SIGEVENT) {
			struct ast_channel *chan;

			/* If we get an event, screen out events that we do not act on.
			 * Otherwise, cancel and go to the simple switch to let it deal with it.
			 */
			res = dahdi_get_event(mtd->pvt->subs[SUB_REAL].dfd);

			switch (res) {
			case DAHDI_EVENT_NEONMWI_ACTIVE:
			case DAHDI_EVENT_NEONMWI_INACTIVE:
			case DAHDI_EVENT_NONE:
			case DAHDI_EVENT_BITSCHANGED:
				break;
			case DAHDI_EVENT_NOALARM:
				mtd->pvt->inalarm = 0;
				ast_log(LOG_NOTICE, "Alarm cleared on channel %d\n", mtd->pvt->channel);
				manager_event(EVENT_FLAG_SYSTEM, "AlarmClear",
					"Channel: %d\r\n", mtd->pvt->channel);
				break;
			case DAHDI_EVENT_ALARM:
				mtd->pvt->inalarm = 1;
				res = get_alarms(mtd->pvt);
				handle_alarms(mtd->pvt, res);
				break; /* What to do on channel alarm ???? -- fall thru intentionally?? */
			default:
				ast_log(LOG_NOTICE, "Got event %d (%s)...  Passing along to ss_thread\n", res, event2str(res));
				callerid_free(cs);

				restore_gains(mtd->pvt);
				mtd->pvt->ringt = mtd->pvt->ringt_base;

				if ((chan = dahdi_new(mtd->pvt, AST_STATE_RING, 0, SUB_REAL, 0, 0))) {
					if (ast_pthread_create_detached(&threadid, NULL, ss_thread, chan)) {
						ast_log(LOG_WARNING, "Unable to start simple switch thread on channel %d\n", mtd->pvt->channel);
						res = tone_zone_play_tone(mtd->pvt->subs[SUB_REAL].dfd, DAHDI_TONE_CONGESTION);
						if (res < 0)
							ast_log(LOG_WARNING, "Unable to play congestion tone on channel %d\n", mtd->pvt->channel);
						ast_hangup(chan);
						goto quit;
					}
					goto quit_no_clean;

				} else {
					ast_log(LOG_WARNING, "Could not create channel to handle call\n");
				}
			}
		} else if (i & DAHDI_IOMUX_READ) {
			if ((res = read(mtd->pvt->subs[SUB_REAL].dfd, mtd->buf, sizeof(mtd->buf))) < 0) {
				if (errno != ELAST) {
					ast_log(LOG_WARNING, "read returned error: %s\n", strerror(errno));
					goto quit;
				}
				break;
			}
			samples += res;
			if (!spill_done) {
				if ((spill_result = callerid_feed(cs, mtd->buf, res, AST_LAW(mtd->pvt))) < 0) {
					ast_log(LOG_WARNING, "CallerID feed failed: %s\n", strerror(errno));
					break;
				} else if (spill_result) {
					spill_done = 1;
				}
			} else {
				/* keep reading data until the energy level drops below the threshold
				   so we don't get another 'trigger' on the remaining carrier signal
				*/
				if (calc_energy(mtd->buf, res, AST_LAW(mtd->pvt)) <= mwilevel)
					break;
			}
			if (samples > (8000 * 4)) /*Termination case - time to give up*/
				break;
		}
	}

	if (spill_result == 1) {
		callerid_get(cs, &name, &number, &flags);
		if (flags & CID_MSGWAITING) {
			ast_log(LOG_NOTICE, "mwi: Have Messages on channel %d\n", mtd->pvt->channel);
			notify_message(mtd->pvt->mailbox, 1);
		} else if (flags & CID_NOMSGWAITING) {
			ast_log(LOG_NOTICE, "mwi: No Messages on channel %d\n", mtd->pvt->channel);
			notify_message(mtd->pvt->mailbox, 0);
		} else {
			ast_log(LOG_NOTICE, "mwi: Status unknown on channel %d\n", mtd->pvt->channel);
		}
	}


quit:
	callerid_free(cs);

	restore_gains(mtd->pvt);

quit_no_clean:
	mtd->pvt->mwimonitoractive = 0;

	ast_free(mtd);

	return NULL;
}

/* destroy a DAHDI channel, identified by its number */
static int dahdi_destroy_channel_bynum(int channel)
{
	struct dahdi_pvt *tmp = NULL;
	struct dahdi_pvt *prev = NULL;

	tmp = iflist;
	while (tmp) {
		if (tmp->channel == channel) {
			int x = DAHDI_FLASH;
			ioctl(tmp->subs[SUB_REAL].dfd, DAHDI_HOOK, &x); /* important to create an event for dahdi_wait_event to register so that all ss_threads terminate */
			destroy_channel(prev, tmp, 1);
			ast_module_unref(ast_module_info->self);
			return RESULT_SUCCESS;
		}
		prev = tmp;
		tmp = tmp->next;
	}
	return RESULT_FAILURE;
}

static int handle_init_event(struct dahdi_pvt *i, int event)
{
	int res;
	pthread_t threadid;
	struct ast_channel *chan;

	/* Handle an event on a given channel for the monitor thread. */

	switch (event) {
	case DAHDI_EVENT_NONE:
	case DAHDI_EVENT_BITSCHANGED:
		break;
	case DAHDI_EVENT_WINKFLASH:
	case DAHDI_EVENT_RINGOFFHOOK:
		if (i->inalarm) break;
		if (i->radio) break;
		/* Got a ring/answer.  What kind of channel are we? */
		switch (i->sig) {
		case SIG_FXOLS:
		case SIG_FXOGS:
		case SIG_FXOKS:
			res = dahdi_set_hook(i->subs[SUB_REAL].dfd, DAHDI_OFFHOOK);
			if (res && (errno == EBUSY))
				break;
			if (i->cidspill) {
				/* Cancel VMWI spill */
				ast_free(i->cidspill);
				i->cidspill = NULL;
			}
			if (i->immediate) {
				dahdi_enable_ec(i);
				/* The channel is immediately up.  Start right away */
				res = tone_zone_play_tone(i->subs[SUB_REAL].dfd, DAHDI_TONE_RINGTONE);
				chan = dahdi_new(i, AST_STATE_RING, 1, SUB_REAL, 0, 0);
				if (!chan) {
					ast_log(LOG_WARNING, "Unable to start PBX on channel %d\n", i->channel);
					res = tone_zone_play_tone(i->subs[SUB_REAL].dfd, DAHDI_TONE_CONGESTION);
					if (res < 0)
						ast_log(LOG_WARNING, "Unable to play congestion tone on channel %d\n", i->channel);
				}
			} else {
				/* Check for callerid, digits, etc */
				chan = dahdi_new(i, AST_STATE_RESERVED, 0, SUB_REAL, 0, 0);
				if (chan) {
					if (has_voicemail(i))
						res = tone_zone_play_tone(i->subs[SUB_REAL].dfd, DAHDI_TONE_STUTTER);
					else
						res = tone_zone_play_tone(i->subs[SUB_REAL].dfd, DAHDI_TONE_DIALTONE);
					if (res < 0)
						ast_log(LOG_WARNING, "Unable to play dialtone on channel %d, do you have defaultzone and loadzone defined?\n", i->channel);
					if (ast_pthread_create_detached(&threadid, NULL, ss_thread, chan)) {
						ast_log(LOG_WARNING, "Unable to start simple switch thread on channel %d\n", i->channel);
						res = tone_zone_play_tone(i->subs[SUB_REAL].dfd, DAHDI_TONE_CONGESTION);
						if (res < 0)
							ast_log(LOG_WARNING, "Unable to play congestion tone on channel %d\n", i->channel);
						ast_hangup(chan);
					}
				} else
					ast_log(LOG_WARNING, "Unable to create channel\n");
			}
			break;
		case SIG_FXSLS:
		case SIG_FXSGS:
		case SIG_FXSKS:
				i->ringt = i->ringt_base;
				/* Fall through */
		case SIG_EMWINK:
		case SIG_FEATD:
		case SIG_FEATDMF:
		case SIG_FEATDMF_TA:
		case SIG_E911:
		case SIG_FGC_CAMA:
		case SIG_FGC_CAMAMF:
		case SIG_FEATB:
		case SIG_EM:
		case SIG_EM_E1:
		case SIG_SFWINK:
		case SIG_SF_FEATD:
		case SIG_SF_FEATDMF:
		case SIG_SF_FEATB:
		case SIG_SF:
				/* Check for callerid, digits, etc */
				if (i->cid_start == CID_START_POLARITY_IN) {
					chan = dahdi_new(i, AST_STATE_PRERING, 0, SUB_REAL, 0, 0);
				} else {
					chan = dahdi_new(i, AST_STATE_RING, 0, SUB_REAL, 0, 0);
				}
				if (chan && ast_pthread_create_detached(&threadid, NULL, ss_thread, chan)) {
					ast_log(LOG_WARNING, "Unable to start simple switch thread on channel %d\n", i->channel);
					res = tone_zone_play_tone(i->subs[SUB_REAL].dfd, DAHDI_TONE_CONGESTION);
					if (res < 0)
						ast_log(LOG_WARNING, "Unable to play congestion tone on channel %d\n", i->channel);
					ast_hangup(chan);
				} else if (!chan) {
					ast_log(LOG_WARNING, "Cannot allocate new structure on channel %d\n", i->channel);
				}
				break;
		default:
			ast_log(LOG_WARNING, "Don't know how to handle ring/answer with signalling %s on channel %d\n", sig2str(i->sig), i->channel);
			res = tone_zone_play_tone(i->subs[SUB_REAL].dfd, DAHDI_TONE_CONGESTION);
			if (res < 0)
					ast_log(LOG_WARNING, "Unable to play congestion tone on channel %d\n", i->channel);
			return -1;
		}
		break;
	case DAHDI_EVENT_NOALARM:
		i->inalarm = 0;
		ast_log(LOG_NOTICE, "Alarm cleared on channel %d\n", i->channel);
		manager_event(EVENT_FLAG_SYSTEM, "AlarmClear",
			"Channel: %d\r\n", i->channel);
		break;
	case DAHDI_EVENT_ALARM:
		i->inalarm = 1;
		res = get_alarms(i);
		handle_alarms(i, res);
		/* fall thru intentionally */
	case DAHDI_EVENT_ONHOOK:
		if (i->radio)
			break;
		/* Back on hook.  Hang up. */
		switch (i->sig) {
		case SIG_FXOLS:
		case SIG_FXOGS:
		case SIG_FEATD:
		case SIG_FEATDMF:
		case SIG_FEATDMF_TA:
		case SIG_E911:
		case SIG_FGC_CAMA:
		case SIG_FGC_CAMAMF:
		case SIG_FEATB:
		case SIG_EM:
		case SIG_EM_E1:
		case SIG_EMWINK:
		case SIG_SF_FEATD:
		case SIG_SF_FEATDMF:
		case SIG_SF_FEATB:
		case SIG_SF:
		case SIG_SFWINK:
		case SIG_FXSLS:
		case SIG_FXSGS:
		case SIG_FXSKS:
		case SIG_GR303FXSKS:
			dahdi_disable_ec(i);
			res = tone_zone_play_tone(i->subs[SUB_REAL].dfd, -1);
			dahdi_set_hook(i->subs[SUB_REAL].dfd, DAHDI_ONHOOK);
			break;
		case SIG_GR303FXOKS:
		case SIG_FXOKS:
			dahdi_disable_ec(i);
			/* Diddle the battery for the zhone */
#ifdef ZHONE_HACK
			dahdi_set_hook(i->subs[SUB_REAL].dfd, DAHDI_OFFHOOK);
			usleep(1);
#endif
			res = tone_zone_play_tone(i->subs[SUB_REAL].dfd, -1);
			dahdi_set_hook(i->subs[SUB_REAL].dfd, DAHDI_ONHOOK);
			break;
		case SIG_PRI:
		case SIG_SS7:
		case SIG_BRI:
		case SIG_BRI_PTMP:
			dahdi_disable_ec(i);
			res = tone_zone_play_tone(i->subs[SUB_REAL].dfd, -1);
			break;
		default:
			ast_log(LOG_WARNING, "Don't know how to handle on hook with signalling %s on channel %d\n", sig2str(i->sig), i->channel);
			res = tone_zone_play_tone(i->subs[SUB_REAL].dfd, -1);
			return -1;
		}
		break;
	case DAHDI_EVENT_POLARITY:
		switch (i->sig) {
		case SIG_FXSLS:
		case SIG_FXSKS:
		case SIG_FXSGS:
			/* We have already got a PR before the channel was
			   created, but it wasn't handled. We need polarity
			   to be REV for remote hangup detection to work.
			   At least in Spain */
			if (i->hanguponpolarityswitch)
				i->polarity = POLARITY_REV;
			if (i->cid_start == CID_START_POLARITY || i->cid_start == CID_START_POLARITY_IN) {
				i->polarity = POLARITY_REV;
				ast_verb(2, "Starting post polarity "
					    "CID detection on channel %d\n",
					    i->channel);
				chan = dahdi_new(i, AST_STATE_PRERING, 0, SUB_REAL, 0, 0);
				if (chan && ast_pthread_create_detached(&threadid, NULL, ss_thread, chan)) {
					ast_log(LOG_WARNING, "Unable to start simple switch thread on channel %d\n", i->channel);
				}
			}
			break;
		default:
			ast_log(LOG_WARNING, "handle_init_event detected "
				"polarity reversal on non-FXO (SIG_FXS) "
				"interface %d\n", i->channel);
		}
		break;
	case DAHDI_EVENT_REMOVED: /* destroy channel */
		ast_log(LOG_NOTICE,
				"Got DAHDI_EVENT_REMOVED. Destroying channel %d\n",
				i->channel);
		dahdi_destroy_channel_bynum(i->channel);
		break;
	case DAHDI_EVENT_NEONMWI_ACTIVE:
		if (i->mwimonitor_neon) {
			notify_message(i->mailbox, 1);
			ast_log(LOG_NOTICE, "NEON MWI set for channel %d, mailbox %s \n", i->channel, i->mailbox);
		}
		break;
	case DAHDI_EVENT_NEONMWI_INACTIVE:
		if (i->mwimonitor_neon) {
			notify_message(i->mailbox, 0);
			ast_log(LOG_NOTICE, "NEON MWI cleared for channel %d, mailbox %s\n", i->channel, i->mailbox);
		}
		break;
	}
	return 0;
}

static void *do_monitor(void *data)
{
	int count, res, res2, spoint, pollres=0;
	struct dahdi_pvt *i;
	struct dahdi_pvt *last = NULL;
	time_t thispass = 0, lastpass = 0;
	int found;
	char buf[1024];
	struct pollfd *pfds=NULL;
	int lastalloc = -1;
	/* This thread monitors all the frame relay interfaces which are not yet in use
	   (and thus do not have a separate thread) indefinitely */
	/* From here on out, we die whenever asked */
#if 0
	if (pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL)) {
		ast_log(LOG_WARNING, "Unable to set cancel type to asynchronous\n");
		return NULL;
	}
	ast_debug(1, "Monitor starting...\n");
#endif
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	for (;;) {
		/* Lock the interface list */
		ast_mutex_lock(&iflock);
		if (!pfds || (lastalloc != ifcount)) {
			if (pfds) {
				ast_free(pfds);
				pfds = NULL;
			}
			if (ifcount) {
				if (!(pfds = ast_calloc(1, ifcount * sizeof(*pfds)))) {
					ast_mutex_unlock(&iflock);
					return NULL;
				}
			}
			lastalloc = ifcount;
		}
		/* Build the stuff we're going to poll on, that is the socket of every
		   dahdi_pvt that does not have an associated owner channel */
		count = 0;
		i = iflist;
		while (i) {
			if ((i->subs[SUB_REAL].dfd > -1) && i->sig && (!i->radio)) {
				if (!i->owner && !i->subs[SUB_REAL].owner && !i->mwimonitoractive) {
					/* This needs to be watched, as it lacks an owner */
					pfds[count].fd = i->subs[SUB_REAL].dfd;
					pfds[count].events = POLLPRI;
					pfds[count].revents = 0;
					/* If we are monitoring for VMWI or sending CID, we need to
					   read from the channel as well */
					if (i->cidspill || i->mwimonitor_fsk)
						pfds[count].events |= POLLIN;
					count++;
				}
			}
			i = i->next;
		}
		/* Okay, now that we know what to do, release the interface lock */
		ast_mutex_unlock(&iflock);

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		pthread_testcancel();
		/* Wait at least a second for something to happen */
		res = poll(pfds, count, 1000);
		pthread_testcancel();
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		/* Okay, poll has finished.  Let's see what happened.  */
		if (res < 0) {
			if ((errno != EAGAIN) && (errno != EINTR))
				ast_log(LOG_WARNING, "poll return %d: %s\n", res, strerror(errno));
			continue;
		}
		/* Alright, lock the interface list again, and let's look and see what has
		   happened */
		ast_mutex_lock(&iflock);
		found = 0;
		spoint = 0;
		lastpass = thispass;
		thispass = time(NULL);
		i = iflist;
		while (i) {
			if (thispass != lastpass) {
				if (!found && ((i == last) || ((i == iflist) && !last))) {
					last = i;
					if (last) {
						if (!last->cidspill && !last->owner && !ast_strlen_zero(last->mailbox) && (thispass - last->onhooktime > 3) &&
							(last->sig & __DAHDI_SIG_FXO)) {
							res = has_voicemail(last);
							if (last->msgstate != res) {
								int x;
								ast_debug(1, "Message status for %s changed from %d to %d on %d\n", last->mailbox, last->msgstate, res, last->channel);
								res2 = ioctl(last->subs[SUB_REAL].dfd, DAHDI_VMWI, &res);
								if (res2)
									ast_log(LOG_DEBUG, "Unable to control message waiting led on channel %d: %s\n", last->channel, strerror(errno));
								x = DAHDI_FLUSH_BOTH;
								res2 = ioctl(last->subs[SUB_REAL].dfd, DAHDI_FLUSH, &x);
								if (res2)
									ast_log(LOG_WARNING, "Unable to flush input on channel %d\n", last->channel);
								if ((last->cidspill = ast_calloc(1, MAX_CALLERID_SIZE))) {
									/* Turn on on hook transfer for 4 seconds */
									x = 4000;
									ioctl(last->subs[SUB_REAL].dfd, DAHDI_ONHOOKTRANSFER, &x);
									last->cidlen = vmwi_generate(last->cidspill, res, 1, AST_LAW(last));
									last->cidpos = 0;
									last->msgstate = res;
									last->onhooktime = thispass;
								}
								found ++;
							}
						}
						last = last->next;
					}
				}
			}
			if ((i->subs[SUB_REAL].dfd > -1) && i->sig) {
				if (i->radio && !i->owner)
				{
					res = dahdi_get_event(i->subs[SUB_REAL].dfd);
					if (res)
					{
						ast_debug(1, "Monitor doohicky got event %s on radio channel %d\n", event2str(res), i->channel);
						/* Don't hold iflock while handling init events */
						ast_mutex_unlock(&iflock);
						handle_init_event(i, res);
						ast_mutex_lock(&iflock);
					}
					i = i->next;
					continue;
				}
				pollres = ast_fdisset(pfds, i->subs[SUB_REAL].dfd, count, &spoint);
				if (pollres & POLLIN) {
					if (i->owner || i->subs[SUB_REAL].owner) {
#ifdef HAVE_PRI
						if (!i->pri)
#endif
							ast_log(LOG_WARNING, "Whoa....  I'm owned but found (%d) in read...\n", i->subs[SUB_REAL].dfd);
						i = i->next;
						continue;
					}
					if (!i->cidspill && !i->mwimonitor_fsk) {
						ast_log(LOG_WARNING, "Whoa....  I'm reading but have no cidspill (%d)...\n", i->subs[SUB_REAL].dfd);
						i = i->next;
						continue;
					}
					res = read(i->subs[SUB_REAL].dfd, buf, sizeof(buf));
					if (res > 0) {
						if (i->mwimonitor_fsk) {
							if (calc_energy((unsigned char *) buf, res, AST_LAW(i)) > mwilevel) {
								pthread_attr_t attr;
								pthread_t threadid;
								struct mwi_thread_data *mtd;

								pthread_attr_init(&attr);
								pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

								ast_log(LOG_DEBUG, "Maybe some MWI on port %d!\n", i->channel);
								if ((mtd = ast_calloc(1, sizeof(*mtd)))) {
									mtd->pvt = i;
									memcpy(mtd->buf, buf, res);
									mtd->len = res;
									if (ast_pthread_create_background(&threadid, &attr, mwi_thread, mtd)) {
										ast_log(LOG_WARNING, "Unable to start mwi thread on channel %d\n", i->channel);
										ast_free(mtd);
									}
									i->mwimonitoractive = 1;
								}
							}
						} else if (i->cidspill) {
							/* We read some number of bytes.  Write an equal amount of data */
							if (res > i->cidlen - i->cidpos)
								res = i->cidlen - i->cidpos;
							res2 = write(i->subs[SUB_REAL].dfd, i->cidspill + i->cidpos, res);
							if (res2 > 0) {
								i->cidpos += res2;
								if (i->cidpos >= i->cidlen) {
									free(i->cidspill);
									i->cidspill = 0;
									i->cidpos = 0;
									i->cidlen = 0;
								}
							} else {
								ast_log(LOG_WARNING, "Write failed: %s\n", strerror(errno));
								i->msgstate = -1;
							}
						}
					} else {
						ast_log(LOG_WARNING, "Read failed with %d: %s\n", res, strerror(errno));
					}
				}
				if (pollres & POLLPRI) {
					if (i->owner || i->subs[SUB_REAL].owner) {
#ifdef HAVE_PRI
						if (!i->pri)
#endif
							ast_log(LOG_WARNING, "Whoa....  I'm owned but found (%d)...\n", i->subs[SUB_REAL].dfd);
						i = i->next;
						continue;
					}
					res = dahdi_get_event(i->subs[SUB_REAL].dfd);
					ast_debug(1, "Monitor doohicky got event %s on channel %d\n", event2str(res), i->channel);
					/* Don't hold iflock while handling init events */
					ast_mutex_unlock(&iflock);
					handle_init_event(i, res);
					ast_mutex_lock(&iflock);
				}
			}
			i=i->next;
		}
		ast_mutex_unlock(&iflock);
	}
	/* Never reached */
	return NULL;

}

static int restart_monitor(void)
{
	/* If we're supposed to be stopped -- stay stopped */
	if (monitor_thread == AST_PTHREADT_STOP)
		return 0;
	ast_mutex_lock(&monlock);
	if (monitor_thread == pthread_self()) {
		ast_mutex_unlock(&monlock);
		ast_log(LOG_WARNING, "Cannot kill myself\n");
		return -1;
	}
	if (monitor_thread != AST_PTHREADT_NULL) {
		/* Wake up the thread */
		pthread_kill(monitor_thread, SIGURG);
	} else {
		/* Start a new monitor */
		if (ast_pthread_create_background(&monitor_thread, NULL, do_monitor, NULL) < 0) {
			ast_mutex_unlock(&monlock);
			ast_log(LOG_ERROR, "Unable to start monitor thread.\n");
			return -1;
		}
	}
	ast_mutex_unlock(&monlock);
	return 0;
}

#ifdef HAVE_PRI
static int pri_resolve_span(int *span, int channel, int offset, struct dahdi_spaninfo *si)
{
	int x;
	int trunkgroup;
	/* Get appropriate trunk group if there is one */
	trunkgroup = pris[*span].mastertrunkgroup;
	if (trunkgroup) {
		/* Select a specific trunk group */
		for (x = 0; x < NUM_SPANS; x++) {
			if (pris[x].trunkgroup == trunkgroup) {
				*span = x;
				return 0;
			}
		}
		ast_log(LOG_WARNING, "Channel %d on span %d configured to use nonexistent trunk group %d\n", channel, *span, trunkgroup);
		*span = -1;
	} else {
		if (pris[*span].trunkgroup) {
			ast_log(LOG_WARNING, "Unable to use span %d implicitly since it is trunk group %d (please use spanmap)\n", *span, pris[*span].trunkgroup);
			*span = -1;
		} else if (pris[*span].mastertrunkgroup) {
			ast_log(LOG_WARNING, "Unable to use span %d implicitly since it is already part of trunk group %d\n", *span, pris[*span].mastertrunkgroup);
			*span = -1;
		} else {
			if (si->totalchans == 31) {
				/* E1 */
				pris[*span].dchannels[0] = 16 + offset;
			} else if (si->totalchans == 24) {
				/* T1 or J1 */
				pris[*span].dchannels[0] = 24 + offset;
			} else if (si->totalchans == 3) {
				/* BRI */
				pris[*span].dchannels[0] = 3 + offset;
			} else {
				ast_log(LOG_WARNING, "Unable to use span %d, since the D-channel cannot be located (unexpected span size of %d channels)\n", *span, si->totalchans);
				*span = -1;
				return 0;
			}
			pris[*span].dchanavail[0] |= DCHAN_PROVISIONED;
			pris[*span].offset = offset;
			pris[*span].span = *span + 1;
		}
	}
	return 0;
}

static int pri_create_trunkgroup(int trunkgroup, int *channels)
{
	struct dahdi_spaninfo si;
	struct dahdi_params p;
	int fd;
	int span;
	int ospan=0;
	int x,y;
	for (x = 0; x < NUM_SPANS; x++) {
		if (pris[x].trunkgroup == trunkgroup) {
			ast_log(LOG_WARNING, "Trunk group %d already exists on span %d, Primary d-channel %d\n", trunkgroup, x + 1, pris[x].dchannels[0]);
			return -1;
		}
	}
	for (y = 0; y < NUM_DCHANS; y++) {
		if (!channels[y])
			break;
		memset(&si, 0, sizeof(si));
		memset(&p, 0, sizeof(p));
		fd = open("/dev/dahdi/channel", O_RDWR);
		if (fd < 0) {
			ast_log(LOG_WARNING, "Failed to open channel: %s\n", strerror(errno));
			return -1;
		}
		x = channels[y];
		if (ioctl(fd, DAHDI_SPECIFY, &x)) {
			ast_log(LOG_WARNING, "Failed to specify channel %d: %s\n", channels[y], strerror(errno));
			dahdi_close(fd);
			return -1;
		}
		if (ioctl(fd, DAHDI_GET_PARAMS, &p)) {
			ast_log(LOG_WARNING, "Failed to get channel parameters for channel %d: %s\n", channels[y], strerror(errno));
			return -1;
		}
		if (ioctl(fd, DAHDI_SPANSTAT, &si)) {
			ast_log(LOG_WARNING, "Failed go get span information on channel %d (span %d): %s\n", channels[y], p.spanno, strerror(errno));
			dahdi_close(fd);
			return -1;
		}
		span = p.spanno - 1;
		if (pris[span].trunkgroup) {
			ast_log(LOG_WARNING, "Span %d is already provisioned for trunk group %d\n", span + 1, pris[span].trunkgroup);
			dahdi_close(fd);
			return -1;
		}
		if (pris[span].pvts[0]) {
			ast_log(LOG_WARNING, "Span %d is already provisioned with channels (implicit PRI maybe?)\n", span + 1);
			dahdi_close(fd);
			return -1;
		}
		if (!y) {
			pris[span].trunkgroup = trunkgroup;
			pris[span].offset = channels[y] - p.chanpos;
			ospan = span;
		}
		pris[ospan].dchannels[y] = channels[y];
		pris[ospan].dchanavail[y] |= DCHAN_PROVISIONED;
		pris[span].span = span + 1;
		dahdi_close(fd);
	}
	return 0;
}

static int pri_create_spanmap(int span, int trunkgroup, int logicalspan)
{
	if (pris[span].mastertrunkgroup) {
		ast_log(LOG_WARNING, "Span %d is already part of trunk group %d, cannot add to trunk group %d\n", span + 1, pris[span].mastertrunkgroup, trunkgroup);
		return -1;
	}
	pris[span].mastertrunkgroup = trunkgroup;
	pris[span].prilogicalspan = logicalspan;
	return 0;
}

#endif

#ifdef HAVE_SS7

static unsigned int parse_pointcode(const char *pcstring)
{
	unsigned int code1, code2, code3;
	int numvals;

	numvals = sscanf(pcstring, "%d-%d-%d", &code1, &code2, &code3);
	if (numvals == 1)
		return code1;
	if (numvals == 3)
		return (code1 << 16) | (code2 << 8) | code3;

	return 0;
}

static struct dahdi_ss7 * ss7_resolve_linkset(int linkset)
{
	if ((linkset < 0) || (linkset >= NUM_SPANS))
		return NULL;
	else
		return &linksets[linkset - 1];
}
#endif /* HAVE_SS7 */

/* converts a DAHDI sigtype to signalling as can be configured from
 * chan_dahdi.conf.
 * While both have basically the same values, this will later be the
 * place to add filters and sanity checks
 */
static int sigtype_to_signalling(int sigtype)
{
        return sigtype;
}

static struct dahdi_pvt *mkintf(int channel, const struct dahdi_chan_conf *conf, struct dahdi_pri *pri, int reloading)
{
	/* Make a dahdi_pvt structure for this interface (or CRV if "pri" is specified) */
	struct dahdi_pvt *tmp = NULL, *tmp2,  *prev = NULL;
	char fn[80];
	struct dahdi_bufferinfo bi;
	int res;
	int span = 0;
	int here = 0;
	int x;
	struct dahdi_pvt **wlist;
	struct dahdi_pvt **wend;
	struct dahdi_params p;

	wlist = &iflist;
	wend = &ifend;

#ifdef HAVE_PRI
	if (pri) {
		wlist = &pri->crvs;
		wend = &pri->crvend;
	}
#endif

	tmp2 = *wlist;
	prev = NULL;

	while (tmp2) {
		if (!tmp2->destroy) {
			if (tmp2->channel == channel) {
				tmp = tmp2;
				here = 1;
				break;
			}
			if (tmp2->channel > channel) {
				break;
			}
		}
		prev = tmp2;
		tmp2 = tmp2->next;
	}

	if (!here && reloading != 1) {
		if (!(tmp = ast_calloc(1, sizeof(*tmp)))) {
			if (tmp)
				free(tmp);
			return NULL;
		}
		ast_mutex_init(&tmp->lock);
		ifcount++;
		for (x = 0; x < 3; x++)
			tmp->subs[x].dfd = -1;
		tmp->channel = channel;
	}

	if (tmp) {
		int chan_sig = conf->chan.sig;
		if (!here) {
			if ((channel != CHAN_PSEUDO) && !pri) {
				int count = 0;
				snprintf(fn, sizeof(fn), "%d", channel);
				/* Open non-blocking */
				tmp->subs[SUB_REAL].dfd = dahdi_open(fn);
				while (tmp->subs[SUB_REAL].dfd < 0 && reloading == 2 && count < 1000) { /* the kernel may not call dahdi_release fast enough for the open flagbit to be cleared in time */
					usleep(1);
					tmp->subs[SUB_REAL].dfd = dahdi_open(fn);
					count++;
				}
				/* Allocate a DAHDI structure */
				if (tmp->subs[SUB_REAL].dfd < 0) {
					ast_log(LOG_ERROR, "Unable to open channel %d: %s\nhere = %d, tmp->channel = %d, channel = %d\n", channel, strerror(errno), here, tmp->channel, channel);
					destroy_dahdi_pvt(&tmp);
					return NULL;
				}
				memset(&p, 0, sizeof(p));
				res = ioctl(tmp->subs[SUB_REAL].dfd, DAHDI_GET_PARAMS, &p);
				if (res < 0) {
					ast_log(LOG_ERROR, "Unable to get parameters: %s\n", strerror(errno));
					destroy_dahdi_pvt(&tmp);
					return NULL;
				}
				if (conf->is_sig_auto)
					chan_sig = sigtype_to_signalling(p.sigtype);
				if (p.sigtype != (chan_sig & 0x3ffff)) {
					ast_log(LOG_ERROR, "Signalling requested on channel %d is %s but line is in %s signalling\n", channel, sig2str(chan_sig), sig2str(p.sigtype));
					destroy_dahdi_pvt(&tmp);
					return NULL;
				}
				tmp->law = p.curlaw;
				tmp->span = p.spanno;
				span = p.spanno - 1;
			} else {
				if (channel == CHAN_PSEUDO)
					chan_sig = 0;
				else if ((chan_sig != SIG_FXOKS) && (chan_sig != SIG_FXSKS)) {
					ast_log(LOG_ERROR, "CRV's must use FXO/FXS Kewl Start (fxo_ks/fxs_ks) signalling only.\n");
					return NULL;
				}
			}
#ifdef HAVE_SS7
			if (chan_sig == SIG_SS7) {
				struct dahdi_ss7 *ss7;
				int clear = 0;
				if (ioctl(tmp->subs[SUB_REAL].dfd, DAHDI_AUDIOMODE, &clear)) {
					ast_log(LOG_ERROR, "Unable to set clear mode on clear channel %d of span %d: %s\n", channel, p.spanno, strerror(errno));
					destroy_dahdi_pvt(&tmp);
					return NULL;
				}

				ss7 = ss7_resolve_linkset(cur_linkset);
				if (!ss7) {
					ast_log(LOG_ERROR, "Unable to find linkset %d\n", cur_linkset);
					destroy_dahdi_pvt(&tmp);
					return NULL;
				}
				if (cur_cicbeginswith < 0) {
					ast_log(LOG_ERROR, "Need to set cicbeginswith for the channels!\n");
					destroy_dahdi_pvt(&tmp);
					return NULL;
				}

				tmp->cic = cur_cicbeginswith++;

				/* DB: Add CIC's DPC information */
				tmp->dpc = cur_defaultdpc;

				tmp->ss7 = ss7;
				tmp->ss7call = NULL;
				ss7->pvts[ss7->numchans++] = tmp;

				ast_copy_string(ss7->internationalprefix, conf->ss7.internationalprefix, sizeof(linksets[span-1].internationalprefix));
				ast_copy_string(ss7->nationalprefix, conf->ss7.nationalprefix, sizeof(linksets[span-1].nationalprefix));
				ast_copy_string(ss7->subscriberprefix, conf->ss7.subscriberprefix, sizeof(linksets[span-1].subscriberprefix));
				ast_copy_string(ss7->unknownprefix, conf->ss7.unknownprefix, sizeof(linksets[span-1].unknownprefix));
				ast_copy_string(ss7->networkroutedprefix, conf->ss7.networkroutedprefix, sizeof(linksets[span-1].networkroutedprefix));

				ss7->called_nai = conf->ss7.called_nai;
				ss7->calling_nai = conf->ss7.calling_nai;
			}
#endif
#ifdef HAVE_PRI
			if ((chan_sig == SIG_PRI) || (chan_sig == SIG_BRI) || (chan_sig == SIG_BRI_PTMP) || (chan_sig == SIG_GR303FXOKS) || (chan_sig == SIG_GR303FXSKS)) {
				int offset;
				int myswitchtype;
				int matchesdchan;
				int x,y;
				offset = 0;
				if (((chan_sig == SIG_PRI) || (chan_sig == SIG_BRI) || (chan_sig == SIG_BRI_PTMP))
						&& ioctl(tmp->subs[SUB_REAL].dfd, DAHDI_AUDIOMODE, &offset)) {
					ast_log(LOG_ERROR, "Unable to set clear mode on clear channel %d of span %d: %s\n", channel, p.spanno, strerror(errno));
					destroy_dahdi_pvt(&tmp);
					return NULL;
				}
				if (span >= NUM_SPANS) {
					ast_log(LOG_ERROR, "Channel %d does not lie on a span I know of (%d)\n", channel, span);
					destroy_dahdi_pvt(&tmp);
					return NULL;
				} else {
					struct dahdi_spaninfo si;
					si.spanno = 0;
					if (ioctl(tmp->subs[SUB_REAL].dfd,DAHDI_SPANSTAT,&si) == -1) {
						ast_log(LOG_ERROR, "Unable to get span status: %s\n", strerror(errno));
						destroy_dahdi_pvt(&tmp);
						return NULL;
					}
					/* Store the logical span first based upon the real span */
					tmp->logicalspan = pris[span].prilogicalspan;
					pri_resolve_span(&span, channel, (channel - p.chanpos), &si);
					if (span < 0) {
						ast_log(LOG_WARNING, "Channel %d: Unable to find locate channel/trunk group!\n", channel);
						destroy_dahdi_pvt(&tmp);
						return NULL;
					}
					if ((chan_sig == SIG_PRI) ||
							(chan_sig == SIG_BRI) ||
							(chan_sig == SIG_BRI_PTMP))
						myswitchtype = conf->pri.switchtype;
					else
						myswitchtype = PRI_SWITCH_GR303_TMC;
					/* Make sure this isn't a d-channel */
					matchesdchan=0;
					for (x = 0; x < NUM_SPANS; x++) {
						for (y = 0; y < NUM_DCHANS; y++) {
							if (pris[x].dchannels[y] == tmp->channel) {
								matchesdchan = 1;
								break;
							}
						}
					}
					offset = p.chanpos;
					if (!matchesdchan) {
						if (pris[span].nodetype && (pris[span].nodetype != conf->pri.nodetype)) {
							ast_log(LOG_ERROR, "Span %d is already a %s node\n", span + 1, pri_node2str(pris[span].nodetype));
							destroy_dahdi_pvt(&tmp);
							return NULL;
						}
						if (pris[span].switchtype && (pris[span].switchtype != myswitchtype)) {
							ast_log(LOG_ERROR, "Span %d is already a %s switch\n", span + 1, pri_switch2str(pris[span].switchtype));
							destroy_dahdi_pvt(&tmp);
							return NULL;
						}
						if ((pris[span].dialplan) && (pris[span].dialplan != conf->pri.dialplan)) {
							ast_log(LOG_ERROR, "Span %d is already a %s dialing plan\n", span + 1, dialplan2str(pris[span].dialplan));
							destroy_dahdi_pvt(&tmp);
							return NULL;
						}
						if (!ast_strlen_zero(pris[span].idledial) && strcmp(pris[span].idledial, conf->pri.idledial)) {
							ast_log(LOG_ERROR, "Span %d already has idledial '%s'.\n", span + 1, conf->pri.idledial);
							destroy_dahdi_pvt(&tmp);
							return NULL;
						}
						if (!ast_strlen_zero(pris[span].idleext) && strcmp(pris[span].idleext, conf->pri.idleext)) {
							ast_log(LOG_ERROR, "Span %d already has idleext '%s'.\n", span + 1, conf->pri.idleext);
							destroy_dahdi_pvt(&tmp);
							return NULL;
						}
						if (pris[span].minunused && (pris[span].minunused != conf->pri.minunused)) {
							ast_log(LOG_ERROR, "Span %d already has minunused of %d.\n", span + 1, conf->pri.minunused);
							destroy_dahdi_pvt(&tmp);
							return NULL;
						}
						if (pris[span].minidle && (pris[span].minidle != conf->pri.minidle)) {
							ast_log(LOG_ERROR, "Span %d already has minidle of %d.\n", span + 1, conf->pri.minidle);
							destroy_dahdi_pvt(&tmp);
							return NULL;
						}
						if (pris[span].numchans >= MAX_CHANNELS) {
							ast_log(LOG_ERROR, "Unable to add channel %d: Too many channels in trunk group %d!\n", channel,
								pris[span].trunkgroup);
							destroy_dahdi_pvt(&tmp);
							return NULL;
						}

						pris[span].sig = chan_sig;
						pris[span].nodetype = conf->pri.nodetype;
						pris[span].switchtype = myswitchtype;
						pris[span].nsf = conf->pri.nsf;
						pris[span].dialplan = conf->pri.dialplan;
						pris[span].localdialplan = conf->pri.localdialplan;
						pris[span].pvts[pris[span].numchans++] = tmp;
						pris[span].minunused = conf->pri.minunused;
						pris[span].minidle = conf->pri.minidle;
						pris[span].overlapdial = conf->pri.overlapdial;
#ifdef HAVE_PRI_INBANDDISCONNECT
						pris[span].inbanddisconnect = conf->pri.inbanddisconnect;
#endif
						pris[span].facilityenable = conf->pri.facilityenable;
						ast_copy_string(pris[span].idledial, conf->pri.idledial, sizeof(pris[span].idledial));
						ast_copy_string(pris[span].idleext, conf->pri.idleext, sizeof(pris[span].idleext));
						ast_copy_string(pris[span].internationalprefix, conf->pri.internationalprefix, sizeof(pris[span].internationalprefix));
						ast_copy_string(pris[span].nationalprefix, conf->pri.nationalprefix, sizeof(pris[span].nationalprefix));
						ast_copy_string(pris[span].localprefix, conf->pri.localprefix, sizeof(pris[span].localprefix));
						ast_copy_string(pris[span].privateprefix, conf->pri.privateprefix, sizeof(pris[span].privateprefix));
						ast_copy_string(pris[span].unknownprefix, conf->pri.unknownprefix, sizeof(pris[span].unknownprefix));
						pris[span].resetinterval = conf->pri.resetinterval;

						tmp->pri = &pris[span];
						tmp->prioffset = offset;
						tmp->call = NULL;
					} else {
						ast_log(LOG_ERROR, "Channel %d is reserved for D-channel.\n", offset);
						destroy_dahdi_pvt(&tmp);
						return NULL;
					}
				}
			} else {
				tmp->prioffset = 0;
			}
#endif
		} else {
			chan_sig = tmp->sig;
			if (tmp->subs[SUB_REAL].dfd > -1) {
				memset(&p, 0, sizeof(p));
				res = ioctl(tmp->subs[SUB_REAL].dfd, DAHDI_GET_PARAMS, &p);
			}
		}
		/* Adjust starttime on loopstart and kewlstart trunks to reasonable values */
		switch (chan_sig) {
		case SIG_FXSKS:
		case SIG_FXSLS:
		case SIG_EM:
		case SIG_EM_E1:
		case SIG_EMWINK:
		case SIG_FEATD:
		case SIG_FEATDMF:
		case SIG_FEATDMF_TA:
		case SIG_FEATB:
		case SIG_E911:
		case SIG_SF:
		case SIG_SFWINK:
		case SIG_FGC_CAMA:
		case SIG_FGC_CAMAMF:
		case SIG_SF_FEATD:
		case SIG_SF_FEATDMF:
		case SIG_SF_FEATB:
			p.starttime = 250;
			break;
		}

		if (tmp->radio) {
			/* XXX Waiting to hear back from Jim if these should be adjustable XXX */
			p.channo = channel;
			p.rxwinktime = 1;
			p.rxflashtime = 1;
			p.starttime = 1;
			p.debouncetime = 5;
		}
		if (!tmp->radio) {
			p.channo = channel;
			/* Override timing settings based on config file */
			if (conf->timing.prewinktime >= 0)
				p.prewinktime = conf->timing.prewinktime;
			if (conf->timing.preflashtime >= 0)
				p.preflashtime = conf->timing.preflashtime;
			if (conf->timing.winktime >= 0)
				p.winktime = conf->timing.winktime;
			if (conf->timing.flashtime >= 0)
				p.flashtime = conf->timing.flashtime;
			if (conf->timing.starttime >= 0)
				p.starttime = conf->timing.starttime;
			if (conf->timing.rxwinktime >= 0)
				p.rxwinktime = conf->timing.rxwinktime;
			if (conf->timing.rxflashtime >= 0)
				p.rxflashtime = conf->timing.rxflashtime;
			if (conf->timing.debouncetime >= 0)
				p.debouncetime = conf->timing.debouncetime;
		}

		/* dont set parms on a pseudo-channel (or CRV) */
		if (tmp->subs[SUB_REAL].dfd >= 0)
		{
			res = ioctl(tmp->subs[SUB_REAL].dfd, DAHDI_SET_PARAMS, &p);
			if (res < 0) {
				ast_log(LOG_ERROR, "Unable to set parameters: %s\n", strerror(errno));
				destroy_dahdi_pvt(&tmp);
				return NULL;
			}
		}
#if 1
		if (!here && (tmp->subs[SUB_REAL].dfd > -1)) {
			memset(&bi, 0, sizeof(bi));
			res = ioctl(tmp->subs[SUB_REAL].dfd, DAHDI_GET_BUFINFO, &bi);
			if (!res) {
				bi.txbufpolicy = conf->chan.buf_policy;
				bi.rxbufpolicy = conf->chan.buf_policy;
				bi.numbufs = conf->chan.buf_no;
				res = ioctl(tmp->subs[SUB_REAL].dfd, DAHDI_SET_BUFINFO, &bi);
				if (res < 0) {
					ast_log(LOG_WARNING, "Unable to set buffer policy on channel %d: %s\n", channel, strerror(errno));
				}
			} else
				ast_log(LOG_WARNING, "Unable to check buffer policy on channel %d: %s\n", channel, strerror(errno));
		}
#endif
		tmp->immediate = conf->chan.immediate;
		tmp->transfertobusy = conf->chan.transfertobusy;
		if (chan_sig & __DAHDI_SIG_FXS) {
			tmp->mwimonitor_fsk = conf->chan.mwimonitor_fsk;
			tmp->mwimonitor_neon = conf->chan.mwimonitor_neon;
		}
		tmp->sig = chan_sig;
		tmp->outsigmod = conf->chan.outsigmod;
		tmp->ringt_base = ringt_base;
		tmp->firstradio = 0;
		if ((chan_sig == SIG_FXOKS) || (chan_sig == SIG_FXOLS) || (chan_sig == SIG_FXOGS))
			tmp->permcallwaiting = conf->chan.callwaiting;
		else
			tmp->permcallwaiting = 0;
		/* Flag to destroy the channel must be cleared on new mkif.  Part of changes for reload to work */
		tmp->destroy = 0;
		tmp->drings = conf->chan.drings;

		/* 10 is a nice default. */
		if (tmp->drings.ringnum[0].range == 0)
			tmp->drings.ringnum[0].range = 10;
		if (tmp->drings.ringnum[1].range == 0)
			tmp->drings.ringnum[1].range = 10;
		if (tmp->drings.ringnum[2].range == 0)
			tmp->drings.ringnum[2].range = 10;

		tmp->usedistinctiveringdetection = usedistinctiveringdetection;
		tmp->callwaitingcallerid = conf->chan.callwaitingcallerid;
		tmp->threewaycalling = conf->chan.threewaycalling;
		tmp->adsi = conf->chan.adsi;
		tmp->use_smdi = conf->chan.use_smdi;
		tmp->permhidecallerid = conf->chan.hidecallerid;
		tmp->callreturn = conf->chan.callreturn;
		tmp->echocancel = conf->chan.echocancel;
		tmp->echotraining = conf->chan.echotraining;
		tmp->pulse = conf->chan.pulse;
		if (tmp->echocancel.head.tap_length) {
			tmp->echocanbridged = conf->chan.echocanbridged;
		} else {
			if (conf->chan.echocanbridged)
				ast_log(LOG_NOTICE, "echocancelwhenbridged requires echocancel to be enabled; ignoring\n");
			tmp->echocanbridged = 0;
		}
		tmp->busydetect = conf->chan.busydetect;
		tmp->busycount = conf->chan.busycount;
		tmp->busy_tonelength = conf->chan.busy_tonelength;
		tmp->busy_quietlength = conf->chan.busy_quietlength;
		tmp->callprogress = conf->chan.callprogress;
		tmp->cancallforward = conf->chan.cancallforward;
		tmp->dtmfrelax = conf->chan.dtmfrelax;
		tmp->callwaiting = tmp->permcallwaiting;
		tmp->hidecallerid = tmp->permhidecallerid;
		tmp->channel = channel;
		tmp->stripmsd = conf->chan.stripmsd;
		tmp->use_callerid = conf->chan.use_callerid;
		tmp->cid_signalling = conf->chan.cid_signalling;
		tmp->cid_start = conf->chan.cid_start;
		tmp->dahditrcallerid = conf->chan.dahditrcallerid;
		tmp->restrictcid = conf->chan.restrictcid;
		tmp->use_callingpres = conf->chan.use_callingpres;
		tmp->priindication_oob = conf->chan.priindication_oob;
		tmp->priexclusive = conf->chan.priexclusive;
		if (tmp->usedistinctiveringdetection) {
			if (!tmp->use_callerid) {
				ast_log(LOG_NOTICE, "Distinctive Ring detect requires 'usecallerid' be on\n");
				tmp->use_callerid = 1;
			}
		}

		if (tmp->cid_signalling == CID_SIG_SMDI) {
			if (!tmp->use_smdi) {
				ast_log(LOG_WARNING, "SMDI callerid requires SMDI to be enabled, enabling...\n");
				tmp->use_smdi = 1;
			}
		}
		if (tmp->use_smdi) {
			tmp->smdi_iface = ast_smdi_interface_find(conf->smdi_port);
			if (!(tmp->smdi_iface)) {
				ast_log(LOG_ERROR, "Invalid SMDI port specfied, disabling SMDI support\n");
				tmp->use_smdi = 0;
			}
		}

		ast_copy_string(tmp->accountcode, conf->chan.accountcode, sizeof(tmp->accountcode));
		tmp->amaflags = conf->chan.amaflags;
		if (!here) {
			tmp->confno = -1;
			tmp->propconfno = -1;
		}
		tmp->canpark = conf->chan.canpark;
		tmp->transfer = conf->chan.transfer;
		ast_copy_string(tmp->defcontext,conf->chan.context,sizeof(tmp->defcontext));
		ast_copy_string(tmp->language, conf->chan.language, sizeof(tmp->language));
		ast_copy_string(tmp->mohinterpret, conf->chan.mohinterpret, sizeof(tmp->mohinterpret));
		ast_copy_string(tmp->mohsuggest, conf->chan.mohsuggest, sizeof(tmp->mohsuggest));
		ast_copy_string(tmp->context, conf->chan.context, sizeof(tmp->context));
		ast_copy_string(tmp->cid_num, conf->chan.cid_num, sizeof(tmp->cid_num));
		tmp->cid_ton = 0;
		ast_copy_string(tmp->cid_name, conf->chan.cid_name, sizeof(tmp->cid_name));
		ast_copy_string(tmp->mailbox, conf->chan.mailbox, sizeof(tmp->mailbox));
		if (channel != CHAN_PSEUDO && !ast_strlen_zero(tmp->mailbox)) {
			char *mailbox, *context;
			mailbox = context = ast_strdupa(tmp->mailbox);
			strsep(&context, "@");
			if (ast_strlen_zero(context))
				context = "default";
			tmp->mwi_event_sub = ast_event_subscribe(AST_EVENT_MWI, mwi_event_cb, NULL,
				AST_EVENT_IE_MAILBOX, AST_EVENT_IE_PLTYPE_STR, mailbox,
				AST_EVENT_IE_CONTEXT, AST_EVENT_IE_PLTYPE_STR, context,
				AST_EVENT_IE_NEWMSGS, AST_EVENT_IE_PLTYPE_EXISTS,
				AST_EVENT_IE_END);
		}
		tmp->msgstate = -1;
		tmp->group = conf->chan.group;
		tmp->callgroup = conf->chan.callgroup;
		tmp->pickupgroup= conf->chan.pickupgroup;
		if (conf->chan.vars) {
			tmp->vars = conf->chan.vars;
		}
		tmp->cid_rxgain = conf->chan.cid_rxgain;
		tmp->rxgain = conf->chan.rxgain;
		tmp->txgain = conf->chan.txgain;
		tmp->tonezone = conf->chan.tonezone;
		tmp->onhooktime = time(NULL);
		if (tmp->subs[SUB_REAL].dfd > -1) {
			set_actual_gain(tmp->subs[SUB_REAL].dfd, 0, tmp->rxgain, tmp->txgain, tmp->law);
			if (tmp->dsp)
				ast_dsp_digitmode(tmp->dsp, DSP_DIGITMODE_DTMF | tmp->dtmfrelax);
			update_conf(tmp);
			if (!here) {
				if ((chan_sig != SIG_BRI) && (chan_sig != SIG_BRI_PTMP) && (chan_sig != SIG_PRI) && (chan_sig != SIG_SS7))
					/* Hang it up to be sure it's good */
					dahdi_set_hook(tmp->subs[SUB_REAL].dfd, DAHDI_ONHOOK);
			}
			ioctl(tmp->subs[SUB_REAL].dfd,DAHDI_SETTONEZONE,&tmp->tonezone);
#ifdef HAVE_PRI
			/* the dchannel is down so put the channel in alarm */
			if (tmp->pri && !pri_is_up(tmp->pri))
				tmp->inalarm = 1;
#endif
			if ((res = get_alarms(tmp)) != DAHDI_ALARM_NONE) {
				tmp->inalarm = 1;
				handle_alarms(tmp, res);
			}
		}

		tmp->polarityonanswerdelay = conf->chan.polarityonanswerdelay;
		tmp->answeronpolarityswitch = conf->chan.answeronpolarityswitch;
		tmp->hanguponpolarityswitch = conf->chan.hanguponpolarityswitch;
		tmp->sendcalleridafter = conf->chan.sendcalleridafter;
		if (!here) {
			tmp->locallyblocked = tmp->remotelyblocked = 0;
			if ((chan_sig == SIG_PRI) || (chan_sig == SIG_BRI) || (chan_sig == SIG_BRI_PTMP) || (chan_sig == SIG_SS7)) {
				tmp->inservice = 0;
				if (chan_sig == SIG_SS7 && tmp->ss7->flags & LINKSET_FLAG_INITIALHWBLO)
					tmp->remotelyblocked |= SS7_BLOCKED_HARDWARE;
			} else /* We default to in service on protocols that don't have a reset */
				tmp->inservice = 1;
		}
	}
	if (tmp && !here) {
		/* nothing on the iflist */
		if (!*wlist) {
			*wlist = tmp;
			tmp->prev = NULL;
			tmp->next = NULL;
			*wend = tmp;
		} else {
			/* at least one member on the iflist */
			struct dahdi_pvt *working = *wlist;

			/* check if we maybe have to put it on the begining */
			if (working->channel > tmp->channel) {
				tmp->next = *wlist;
				tmp->prev = NULL;
				(*wlist)->prev = tmp;
				*wlist = tmp;
			} else {
			/* go through all the members and put the member in the right place */
				while (working) {
					/* in the middle */
					if (working->next) {
						if (working->channel < tmp->channel && working->next->channel > tmp->channel) {
							tmp->next = working->next;
							tmp->prev = working;
							working->next->prev = tmp;
							working->next = tmp;
							break;
						}
					} else {
					/* the last */
						if (working->channel < tmp->channel) {
							working->next = tmp;
							tmp->next = NULL;
							tmp->prev = working;
							*wend = tmp;
							break;
						}
					}
					working = working->next;
				}
			}
		}
	}
	return tmp;
}

static inline int available(struct dahdi_pvt *p, int channelmatch, ast_group_t groupmatch, int *busy, int *channelmatched, int *groupmatched)
{
	int res;
	struct dahdi_params par;

	/* First, check group matching */
	if (groupmatch) {
		if ((p->group & groupmatch) != groupmatch)
			return 0;
		*groupmatched = 1;
	}
	/* Check to see if we have a channel match */
	if (channelmatch != -1) {
		if (p->channel != channelmatch)
			return 0;
		*channelmatched = 1;
	}
	/* We're at least busy at this point */
	if (busy) {
		if ((p->sig == SIG_FXOKS) || (p->sig == SIG_FXOLS) || (p->sig == SIG_FXOGS))
			*busy = 1;
	}
	/* If do not disturb, definitely not */
	if (p->dnd)
		return 0;
	/* If guard time, definitely not */
	if (p->guardtime && (time(NULL) < p->guardtime))
		return 0;

	if (p->locallyblocked || p->remotelyblocked)
		return 0;

	/* If no owner definitely available */
	if (!p->owner) {
#ifdef HAVE_PRI
		/* Trust PRI */
		if (p->pri) {
			if (p->resetting || p->call)
				return 0;
			else
				return 1;
		}
#endif
#ifdef HAVE_SS7
		/* Trust SS7 */
		if (p->ss7) {
			if (p->ss7call || !p->inservice)
				return 0;
			else
				return 1;
		}
#endif
		if (!(p->radio || (p->oprmode < 0)))
		{
			if (!p->sig || (p->sig == SIG_FXSLS))
				return 1;
			/* Check hook state */
			if (p->subs[SUB_REAL].dfd > -1) {
				memset(&par, 0, sizeof(par));
				res = ioctl(p->subs[SUB_REAL].dfd, DAHDI_GET_PARAMS, &par);
			} else {
				/* Assume not off hook on CVRS */
				res = 0;
				par.rxisoffhook = 0;
			}
			if (res) {
				ast_log(LOG_WARNING, "Unable to check hook state on channel %d: %s\n", p->channel, strerror(errno));
			} else if ((p->sig == SIG_FXSKS) || (p->sig == SIG_FXSGS)) {
				/* When "onhook" that means no battery on the line, and thus
				  it is out of service..., if it's on a TDM card... If it's a channel
				  bank, there is no telling... */
				if (par.rxbits > -1)
					return 1;
				if (par.rxisoffhook)
					return 1;
				else
					return 0;
			} else if (par.rxisoffhook) {
				ast_debug(1, "Channel %d off hook, can't use\n", p->channel);
				/* Not available when the other end is off hook */
				return 0;
			}
		}
		return 1;
	}

	/* If it's not an FXO, forget about call wait */
	if ((p->sig != SIG_FXOKS) && (p->sig != SIG_FXOLS) && (p->sig != SIG_FXOGS))
		return 0;

	if (!p->callwaiting) {
		/* If they don't have call waiting enabled, then for sure they're unavailable at this point */
		return 0;
	}

	if (p->subs[SUB_CALLWAIT].dfd > -1) {
		/* If there is already a call waiting call, then we can't take a second one */
		return 0;
	}

	if ((p->owner->_state != AST_STATE_UP) &&
	    ((p->owner->_state != AST_STATE_RINGING) || p->outgoing)) {
		/* If the current call is not up, then don't allow the call */
		return 0;
	}
	if ((p->subs[SUB_THREEWAY].owner) && (!p->subs[SUB_THREEWAY].inthreeway)) {
		/* Can't take a call wait when the three way calling hasn't been merged yet. */
		return 0;
	}
	/* We're cool */
	return 1;
}

static struct dahdi_pvt *chandup(struct dahdi_pvt *src)
{
	struct dahdi_pvt *p;
	struct dahdi_bufferinfo bi;
	int res;

	if ((p = ast_malloc(sizeof(*p)))) {
		memcpy(p, src, sizeof(struct dahdi_pvt));
		ast_mutex_init(&p->lock);
		p->subs[SUB_REAL].dfd = dahdi_open("/dev/dahdi/pseudo");
		/* Allocate a dahdi structure */
		if (p->subs[SUB_REAL].dfd < 0) {
			ast_log(LOG_ERROR, "Unable to dup channel: %s\n",  strerror(errno));
			destroy_dahdi_pvt(&p);
			return NULL;
		}
		res = ioctl(p->subs[SUB_REAL].dfd, DAHDI_GET_BUFINFO, &bi);
		if (!res) {
			bi.txbufpolicy = src->buf_policy;
			bi.rxbufpolicy = src->buf_policy;
			bi.numbufs = src->buf_no;
			res = ioctl(p->subs[SUB_REAL].dfd, DAHDI_SET_BUFINFO, &bi);
			if (res < 0) {
				ast_log(LOG_WARNING, "Unable to set buffer policy on dup channel: %s\n", strerror(errno));
			}
		} else
			ast_log(LOG_WARNING, "Unable to check buffer policy on dup channel: %s\n", strerror(errno));
	}
	p->destroy = 1;
	p->next = iflist;
	p->prev = NULL;
	iflist = p;
	if (iflist->next)
		iflist->next->prev = p;
	return p;
}


#ifdef HAVE_PRI
static int pri_find_empty_chan(struct dahdi_pri *pri, int backwards)
{
	int x;
	if (backwards)
		x = pri->numchans;
	else
		x = 0;
	for (;;) {
		if (backwards && (x < 0))
			break;
		if (!backwards && (x >= pri->numchans))
			break;
		if (pri->pvts[x] && !pri->pvts[x]->inalarm && !pri->pvts[x]->owner) {
			ast_debug(1, "Found empty available channel %d/%d\n",
				pri->pvts[x]->logicalspan, pri->pvts[x]->prioffset);
			return x;
		}
		if (backwards)
			x--;
		else
			x++;
	}
	return -1;
}
#endif

static struct ast_channel *dahdi_request(const char *type, int format, void *data, int *cause)
{
	ast_group_t groupmatch = 0;
	int channelmatch = -1;
	int roundrobin = 0;
	int callwait = 0;
	int busy = 0;
	struct dahdi_pvt *p;
	struct ast_channel *tmp = NULL;
	char *dest=NULL;
	int x;
	char *s;
	char opt=0;
	int res=0, y=0;
	int backwards = 0;
#ifdef HAVE_PRI
	int crv;
	int bearer = -1;
	int trunkgroup;
	struct dahdi_pri *pri=NULL;
#endif
	struct dahdi_pvt *exit, *start, *end;
	ast_mutex_t *lock;
	int channelmatched = 0;
	int groupmatched = 0;

	/* Assume we're locking the iflock */
	lock = &iflock;
	start = iflist;
	end = ifend;
	if (data) {
		dest = ast_strdupa((char *)data);
	} else {
		ast_log(LOG_WARNING, "Channel requested with no data\n");
		return NULL;
	}
	if (toupper(dest[0]) == 'G' || toupper(dest[0])=='R') {
		/* Retrieve the group number */
		char *stringp=NULL;
		stringp=dest + 1;
		s = strsep(&stringp, "/");
		if ((res = sscanf(s, "%d%c%d", &x, &opt, &y)) < 1) {
			ast_log(LOG_WARNING, "Unable to determine group for data %s\n", (char *)data);
			return NULL;
		}
		groupmatch = ((ast_group_t) 1 << x);
		if (toupper(dest[0]) == 'G') {
			if (dest[0] == 'G') {
				backwards = 1;
				p = ifend;
			} else
				p = iflist;
		} else {
			if (dest[0] == 'R') {
				backwards = 1;
				p = round_robin[x]?round_robin[x]->prev:ifend;
				if (!p)
					p = ifend;
			} else {
				p = round_robin[x]?round_robin[x]->next:iflist;
				if (!p)
					p = iflist;
			}
			roundrobin = 1;
		}
	} else {
		char *stringp=NULL;
		stringp=dest;
		s = strsep(&stringp, "/");
		p = iflist;
		if (!strcasecmp(s, "pseudo")) {
			/* Special case for pseudo */
			x = CHAN_PSEUDO;
			channelmatch = x;
		}
#ifdef HAVE_PRI
		else if ((res = sscanf(s, "%d:%d%c%d", &trunkgroup, &crv, &opt, &y)) > 1) {
			if ((trunkgroup < 1) || (crv < 1)) {
				ast_log(LOG_WARNING, "Unable to determine trunk group and CRV for data %s\n", (char *)data);
				return NULL;
			}
			res--;
			for (x = 0; x < NUM_SPANS; x++) {
				if (pris[x].trunkgroup == trunkgroup) {
					pri = pris + x;
					lock = &pri->lock;
					start = pri->crvs;
					end = pri->crvend;
					break;
				}
			}
			if (!pri) {
				ast_log(LOG_WARNING, "Unable to find trunk group %d\n", trunkgroup);
				return NULL;
			}
			channelmatch = crv;
			p = pris[x].crvs;
		}
#endif
		else if ((res = sscanf(s, "%d%c%d", &x, &opt, &y)) < 1) {
			ast_log(LOG_WARNING, "Unable to determine channel for data %s\n", (char *)data);
			return NULL;
		} else {
			channelmatch = x;
		}
	}
	/* Search for an unowned channel */
	ast_mutex_lock(lock);
	exit = p;
	while (p && !tmp) {
		if (roundrobin)
			round_robin[x] = p;
#if 0
		ast_verbose("name = %s, %d, %d, %d\n",p->owner ? p->owner->name : "<none>", p->channel, channelmatch, groupmatch);
#endif

		if (p && available(p, channelmatch, groupmatch, &busy, &channelmatched, &groupmatched)) {
			ast_debug(1, "Using channel %d\n", p->channel);
			if (p->inalarm)
				goto next;

			callwait = (p->owner != NULL);
#ifdef HAVE_PRI
			if (pri && (p->subs[SUB_REAL].dfd < 0)) {
				if (p->sig != SIG_FXSKS) {
					/* Gotta find an actual channel to use for this
					   CRV if this isn't a callwait */
					bearer = pri_find_empty_chan(pri, 0);
					if (bearer < 0) {
						ast_log(LOG_NOTICE, "Out of bearer channels on span %d for call to CRV %d:%d\n", pri->span, trunkgroup, crv);
						p = NULL;
						break;
					}
					pri_assign_bearer(p, pri, pri->pvts[bearer]);
				} else {
					if (alloc_sub(p, 0)) {
						ast_log(LOG_NOTICE, "Failed to allocate place holder pseudo channel!\n");
						p = NULL;
						break;
					} else
						ast_debug(1, "Allocated placeholder pseudo channel\n");

					p->pri = pri;
				}
			}
#endif
			if (p->channel == CHAN_PSEUDO) {
				p = chandup(p);
				if (!p) {
					break;
				}
			}
			if (p->owner) {
				if (alloc_sub(p, SUB_CALLWAIT)) {
					p = NULL;
					break;
				}
			}
			p->outgoing = 1;
			tmp = dahdi_new(p, AST_STATE_RESERVED, 0, p->owner ? SUB_CALLWAIT : SUB_REAL, 0, 0);
#ifdef HAVE_PRI
			if (p->bearer) {
				/* Log owner to bearer channel, too */
				p->bearer->owner = tmp;
			}
#endif
			/* Make special notes */
			if (res > 1) {
				if (opt == 'c') {
					/* Confirm answer */
					p->confirmanswer = 1;
				} else if (opt == 'r') {
					/* Distinctive ring */
					if (res < 3)
						ast_log(LOG_WARNING, "Distinctive ring missing identifier in '%s'\n", (char *)data);
					else
						p->distinctivering = y;
				} else if (opt == 'd') {
					/* If this is an ISDN call, make it digital */
					p->digital = 1;
					if (tmp)
						tmp->transfercapability = AST_TRANS_CAP_DIGITAL;
				} else {
					ast_log(LOG_WARNING, "Unknown option '%c' in '%s'\n", opt, (char *)data);
				}
			}
			/* Note if the call is a call waiting call */
			if (tmp && callwait)
				tmp->cdrflags |= AST_CDR_CALLWAIT;
			break;
		}
next:
		if (backwards) {
			p = p->prev;
			if (!p)
				p = end;
		} else {
			p = p->next;
			if (!p)
				p = start;
		}
		/* stop when you roll to the one that we started from */
		if (p == exit)
			break;
	}
	ast_mutex_unlock(lock);
	restart_monitor();
	if (callwait)
		*cause = AST_CAUSE_BUSY;
	else if (!tmp) {
		if (channelmatched) {
			if (busy)
				*cause = AST_CAUSE_BUSY;
		} else if (groupmatched) {
			*cause = AST_CAUSE_CONGESTION;
		}
	}

	return tmp;
}

#if defined(HAVE_PRI) || defined(HAVE_SS7)
static int dahdi_setlaw(int dfd, int law)
{
	return ioctl(dfd, DAHDI_SETLAW, &law);
}
#endif

#ifdef HAVE_SS7

static int ss7_find_cic(struct dahdi_ss7 *linkset, int cic, unsigned int dpc)
{
	int i;
	int winner = -1;
	for (i = 0; i < linkset->numchans; i++) {
		if (linkset->pvts[i] && (linkset->pvts[i]->dpc == dpc && linkset->pvts[i]->cic == cic)) {
			winner = i;
			break;
		}
	}
	return winner;
}

static void ss7_check_range(struct dahdi_ss7 *linkset, int startcic, int endcic, unsigned int dpc, unsigned char *state)
{
	int cic;

	for(cic = startcic; cic <= endcic; cic++)
		if(state[cic - startcic] && ss7_find_cic(linkset, cic, dpc) == -1)
			state[cic - startcic] = 0;
}

static int ss7_find_cic_range(struct dahdi_ss7 *linkset, int startcic, int endcic, unsigned int dpc)
{
	int x, found = 0;

	for(x = 0; x < linkset->numchans; x++)
		if (linkset->pvts[x] && (linkset->pvts[x]->dpc == dpc && linkset->pvts[x]->cic >= startcic && linkset->pvts[x]->cic <= endcic))
			found++;
	if(found == endcic - startcic + 1)
		return  1;
	else
		return 0;
}

static void ss7_handle_cqm(struct dahdi_ss7 *linkset, int startcic, int endcic, unsigned int dpc)
{
	unsigned char status[32];
	struct dahdi_pvt *p = NULL;
	int i, offset;

	for (i = 0; i < linkset->numchans; i++) {
		if (linkset->pvts[i] && (linkset->pvts[i]->dpc == dpc && ((linkset->pvts[i]->cic >= startcic) && (linkset->pvts[i]->cic <= endcic)))) {
			p = linkset->pvts[i];
			offset = p->cic - startcic;
			status[offset] = 0;
			if (p->locallyblocked)
				status[offset] |= (1 << 0) | (1 << 4);
			if (p->remotelyblocked)
				status[offset] |= (1 << 1) | (1 << 5);
			if (p->ss7call) {
				if (p->outgoing)
					status[offset] |= (1 << 3);
				else
					status[offset] |= (1 << 2);
			} else
				status[offset] |= 0x3 << 2;
		}
	}

	if (p)
		isup_cqr(linkset->ss7, startcic, endcic, dpc, status);
	else
		ast_log(LOG_WARNING, "Could not find any equipped circuits within CQM CICs\n");

}

static inline void ss7_block_cics(struct dahdi_ss7 *linkset, int startcic, int endcic, unsigned int dpc, unsigned char state[], int block, int remotely, int type)
{
	int i, offset = 0;
	for (i = 0; i < linkset->numchans; i++) {
		if (linkset->pvts[i] && (linkset->pvts[i]->dpc == dpc && ((linkset->pvts[i]->cic >= startcic) && (linkset->pvts[i]->cic <= endcic)))) {
			if (state) {
				if (state[offset]) {
					if(linkset->pvts[i]->cic != startcic)
						ast_mutex_lock(&linkset->pvts[i]->lock);

					if (block) {
						if (remotely)
							linkset->pvts[i]->remotelyblocked |= type;
						else
							linkset->pvts[i]->locallyblocked |= type;
					} else {
						if (remotely)
							linkset->pvts[i]->remotelyblocked &= ~type;
						else
							linkset->pvts[i]->locallyblocked &= ~type;
					}

					if(linkset->pvts[i]->owner && linkset->pvts[i]->owner->_state == AST_STATE_DIALING && !linkset->pvts[i]->proceeding) {
						linkset->pvts[i]->owner->hangupcause = SS7_CAUSE_TRY_AGAIN;
						linkset->pvts[i]->owner->_softhangup |= AST_SOFTHANGUP_DEV;
					}

					if(linkset->pvts[i]->cic != startcic)
						ast_mutex_unlock(&linkset->pvts[i]->lock);
				}
				offset++;
			} else {
				if (block) {
					if (remotely)
						linkset->pvts[i]->remotelyblocked |= type;
					else
						linkset->pvts[i]->locallyblocked |= type;
				} else {
					if (remotely)
						linkset->pvts[i]->remotelyblocked &= ~type;
					else
						linkset->pvts[i]->locallyblocked &= ~type;
				}
			}
		}
	} /* for */
}

static void ss7_inservice(struct dahdi_ss7 *linkset, int startcic, int endcic, unsigned int dpc)
{
	int i;

	for (i = 0; i < linkset->numchans; i++) {
		if (linkset->pvts[i] && (linkset->pvts[i]->dpc == dpc && ((linkset->pvts[i]->cic >= startcic) && (linkset->pvts[i]->cic <= endcic))))
			linkset->pvts[i]->inservice = 1;
	}
}

static void ss7_reset_linkset(struct dahdi_ss7 *linkset)
{
	int i, startcic = -1, endcic, dpc;
	struct dahdi_pvt *p;

	if (linkset->numchans <= 0)
		return;

	startcic = linkset->pvts[0]->cic;
	p = linkset->pvts[0];
	/* DB: CIC's DPC fix */
	dpc = linkset->pvts[0]->dpc;

	for (i = 0; i < linkset->numchans; i++) {
		if (linkset->pvts[i+1] && linkset->pvts[i+1]->dpc == dpc && ((linkset->pvts[i+1]->cic - linkset->pvts[i]->cic) == 1) && (linkset->pvts[i]->cic - startcic < 31)) {
			continue;
		} else {
			endcic = linkset->pvts[i]->cic;
			ast_verbose("Resetting CICs %d to %d\n", startcic, endcic);
			if(!ss7_find_alloc_call(p))
				ast_log(LOG_ERROR, "Unable allocate new ss7call\n");
			else
				isup_grs(linkset->ss7, p->ss7call, endcic);

			/* DB: CIC's DPC fix */
			if (linkset->pvts[i+1]) {
				startcic = linkset->pvts[i+1]->cic;
				dpc = linkset->pvts[i+1]->dpc;
				p = linkset->pvts[i+1];
			}
		}
	}
}

static void dahdi_loopback(struct dahdi_pvt *p, int enable)
{
	if (p->loopedback != enable) {
		if (ioctl(p->subs[SUB_REAL].dfd, DAHDI_LOOPBACK, &enable)) {
			ast_log(LOG_WARNING, "Unable to set loopback on channel %d: %s\n", p->channel, strerror(errno));
			return;
		}
		p->loopedback = enable;
	}
}

/* call me with locked p !!! */
static int ss7_do_rsc(struct dahdi_pvt *p) {
	if(!p || !p->ss7call)
		return 0;

	isup_rsc(p->ss7->ss7, p->ss7call);

	if(p->locallyblocked == SS7_BLOCKED_MAINTENANCE)
		isup_blo(p->ss7->ss7, p->ss7call);
	else
		p->locallyblocked = 0;

	return 1;
}

/* call me with locked p !!! */
static int ss7_start_rsc(struct dahdi_pvt *p) {
	if(!p)
		return 0;

	if(!ss7_find_alloc_call(p))
		return 0;

	p->remotelyblocked = 0;
	p->inservice = 0;
	dahdi_loopback(p, 0);

	if(p->owner) {
		p->owner->hangupcause = AST_CAUSE_NORMAL_CLEARING;
		p->owner->_softhangup |= AST_SOFTHANGUP_DEV;
		p->do_hangup = SS7_HANGUP_SEND_RSC;
		return 1;
	} else
		ss7_do_rsc(p);

	return 1;
}

static int ss7_clear_channels(struct dahdi_pvt *p, int endcic, int do_hangup) {
	int i, chanpos, clean = 1;
	struct dahdi_pvt *p_cur;

	if (!p || !p->ss7call)
		return 0;

	for (i = p->cic; i <= endcic; i++)	{
		chanpos = ss7_find_cic(p->ss7, i, p->dpc);
		p_cur = p->ss7->pvts[chanpos];

		if (p_cur != p)
			ast_mutex_lock(&p_cur->lock);

		p_cur->inservice = 0;

		if (p_cur->owner) {
			p_cur->owner->hangupcause = AST_CAUSE_NORMAL_CLEARING;
			p_cur->owner->_softhangup |= AST_SOFTHANGUP_DEV;
			if (p != p_cur)
				p_cur->do_hangup = do_hangup;
			else
				p_cur->do_hangup = SS7_HANGUP_DO_NOTHING;
			clean = 0;
		} else if (p_cur->ss7call && p != p_cur) {
			isup_free_call(p_cur->ss7->ss7, p_cur->ss7call);
			p_cur->ss7call = NULL;
		}
		if (p_cur != p)
			ast_mutex_unlock(&p_cur->lock);
	}

	return clean;
}

/* XXX: This function is assumed to be called with the private channel lock and linkset lock held */
static void ss7_start_call(struct dahdi_pvt *p, struct dahdi_ss7 *linkset)
{
	struct ss7 *ss7 = linkset->ss7;
	int res;
	int law = 1;
	struct ast_channel *c;
	char tmp[256];
	char *strp;

	if (ioctl(p->subs[SUB_REAL].dfd, DAHDI_AUDIOMODE, &law) == -1)
		ast_log(LOG_WARNING, "Unable to set audio mode on channel %d to %d: %s\n", p->channel, law, strerror(errno));

	if (linkset->type == SS7_ITU)
		law = DAHDI_LAW_ALAW;
	else
		law = DAHDI_LAW_MULAW;

	res = dahdi_setlaw(p->subs[SUB_REAL].dfd, law);
	if (res < 0)
		ast_log(LOG_WARNING, "Unable to set law on channel %d\n", p->channel);

	isup_set_echocontrol(p->ss7call, (linkset->flags & LINKSET_FLAG_DEFAULTECHOCONTROL) ? 1 : 0);

	if (!(linkset->flags & LINKSET_FLAG_EXPLICITACM)) {
		p->proceeding = 1;
		isup_acm(ss7, p->ss7call);
	}

	ast_mutex_unlock(&linkset->lock);
	c = dahdi_new(p, AST_STATE_RING, 0, SUB_REAL, law, 0); /* The startpbx is sometimes faster than we set up the variables!!! */

	if (!c) {
		ast_log(LOG_WARNING, "Unable to start PBX on CIC %d\n", p->cic);
		/* Holding this lock is assumed entering the function */
		ast_mutex_lock(&linkset->lock);
		isup_rel(p->ss7->ss7, p->ss7call, AST_CAUSE_SWITCH_CONGESTION);
		return;
	} else
		ast_verb(3, "Accepting call to '%s' on CIC %d\n", p->exten, p->cic);

	 if(!p->echocontrol_ind || !(p->ss7->flags & LINKSET_FLAG_USEECHOCONTROL)) {
		dahdi_enable_ec(p);
		dahdi_train_ec(p);
	 }

	/* We only reference these variables in the context of the ss7_linkset function
	 * when receiving either and IAM or a COT message.  Since they are only accessed
	 * from this context, we should be safe to unlock around them */

	ast_mutex_unlock(&p->lock);

	if (!ast_strlen_zero(p->charge_number)) {
		pbx_builtin_setvar_helper(c, "SS7_CHARGE_NUMBER", p->charge_number);
		/* Clear this after we set it */
		p->charge_number[0] = 0;
	}
	if (!ast_strlen_zero(p->gen_add_number)) {
		pbx_builtin_setvar_helper(c, "SS7_GENERIC_ADDRESS", p->gen_add_number);
		/* Clear this after we set it */
		p->gen_add_number[0] = 0;
	}
	if (!ast_strlen_zero(p->jip_number)) {
		pbx_builtin_setvar_helper(c, "SS7_JIP", p->jip_number);
		/* Clear this after we set it */
		p->jip_number[0] = 0;
	}
	if (!ast_strlen_zero(p->gen_dig_number)) {
		pbx_builtin_setvar_helper(c, "SS7_GENERIC_DIGITS", p->gen_dig_number);
		/* Clear this after we set it */
		p->gen_dig_number[0] = 0;
	}
	if (!ast_strlen_zero(p->orig_called_num)) {
		pbx_builtin_setvar_helper(c, "SS7_ORIG_CALLED_NUM", p->orig_called_num);
		/* Clear this after we set it */
		p->orig_called_num[0] = 0;
	}

	snprintf(tmp, sizeof(tmp), "%d", p->gen_dig_type);
	pbx_builtin_setvar_helper(c, "SS7_GENERIC_DIGTYPE", tmp);
	/* Clear this after we set it */
	p->gen_dig_type = 0;

	snprintf(tmp, sizeof(tmp), "%d", p->gen_dig_scheme);
	pbx_builtin_setvar_helper(c, "SS7_GENERIC_DIGSCHEME", tmp);
	/* Clear this after we set it */
	p->gen_dig_scheme = 0;

	if (!ast_strlen_zero(p->lspi_ident)) {
		pbx_builtin_setvar_helper(c, "SS7_LSPI_IDENT", p->lspi_ident);
		/* Clear this after we set it */
		p->lspi_ident[0] = 0;
	}

	snprintf(tmp, sizeof(tmp), "%d", p->call_ref_ident);
	pbx_builtin_setvar_helper(c, "SS7_CALLREF_IDENT", tmp);
	/* Clear this after we set it */
	p->call_ref_ident = 0;

	snprintf(tmp, sizeof(tmp), "%d", p->call_ref_pc);
	pbx_builtin_setvar_helper(c, "SS7_CALLREF_PC", tmp);
	/* Clear this after we set it */
	p->call_ref_pc = 0;

	snprintf(tmp, sizeof(tmp), "%d", p->calling_party_cat);
	pbx_builtin_setvar_helper(c, "SS7_CALLING_PARTY_CATEGORY", tmp);
	/* Clear this after we set it */
	p->calling_party_cat = 0;

	if (!ast_strlen_zero(p->redirecting_num)) {
		pbx_builtin_setvar_helper(c, "SS7_REDIRECTING_NUMBER", p->redirecting_num);
		/* Clear this after we set it */
		p->redirecting_num[0] = 0;
	}
	if (!ast_strlen_zero(p->generic_name)) {
		pbx_builtin_setvar_helper(c, "SS7_GENERIC_NAME", p->generic_name);
		/* Clear this after we set it */
		p->generic_name[0] = 0;
	}
	if (p->redirect_counter) {
		char redirect_counter_str[8];
		sprintf(redirect_counter_str, "%u", p->redirect_counter);
		pbx_builtin_setvar_helper(c, "SS7_REDIRECT_COUNTER", redirect_counter_str);
		/* Clear this after we set it */
		p->redirect_counter = 0;
	}
	if (p->redirect_info) {
		switch (p->redirect_info_ind) {
		case 0:
				strp = "NO_REDIRECTION";
			break;
		case 1:
			strp = "CALL_REROUTED_PRES_ALLOWED";
			break;
		case 2:
			strp = "CALL_REROUTED_INFO_RESTRICTED";
			break;
		case 3:
			strp = "CALL_DIVERTED_PRES_ALLOWED";
			break;
		case 4:
			strp = "CALL_DIVERTED_INFO_RESTRICTED";
			break;
		case 5:
			strp = "CALL_REROUTED_PRES_RESTRICTED";
			break;
		case 6:
			strp = "CALL_DIVERTED_PRES_RESTRICTED";
			break;
		case 7:
			strp = "SPARE";
			break;
		default:
			strp = "NO_REDIRECTION";
		}
		pbx_builtin_setvar_helper(c, "SS7_REDIRECT_INFO_IND", strp);

		switch (p->redirect_info_orig_reas) {
		case 0:
				strp = "UNKNOWN";
				break;
		case 1:
				strp = "USER-BUSY";
				break;
		case 2:
				strp = "NO-ANSWER";
				break;
		case 3:
				strp = "UNCONDITIONAL";
				break;
		default:
				strp = "UNKNOWN";
		}
		pbx_builtin_setvar_helper(c, "SS7_REDIRECT_INFO_ORIG_REAS", strp);

	switch (p->redirect_info_reas) {
		case 0:
				strp = "UNKNOWN";
				break;
		case 1:
				strp = "USER-BUSY";
				break;
		case 2:
				strp = "NO-ANSWER";
				break;
		case 3:
				strp = "UNCONDITIONAL";
				break;
		case 4:
				strp = "DEFLECTION_DURING_ALERTING";
				break;
		case 5:
				strp = "DEFLECTION_IMMEDIATE_RESPONSE";
				break;
		case 6:
				strp = "UNAVAILABLE";
				break;
		default:
				strp = "UNKNOWN";
			break;
		}
		pbx_builtin_setvar_helper(c, "SS7_REDIRECT_INFO_REAS", strp);

		snprintf(tmp, sizeof(tmp), "%d", p->redirect_info_counter);
		pbx_builtin_setvar_helper(c, "SS7_REDIRECT_INFO_COUNTER", tmp);
	}
	if (p->cug_indicator != ISUP_CUG_NON) {
		sprintf(tmp, "%d", p->cug_interlock_code);
		pbx_builtin_setvar_helper(c, "SS7_CUG_INTERLOCK_CODE", tmp);

		switch (p->cug_indicator) {
		case ISUP_CUG_NON:
			strp = "NON_CUG";
			break;
		case ISUP_CUG_OUTGOING_ALLOWED:
			strp = "OUTGOING_ALLOWED";
			break;
		case ISUP_CUG_OUTGOING_NOT_ALLOWED:
			strp = "OUTGOING_NOT_ALLOWED";
			break;
		default:
			strp = "SPARE";
		}
		pbx_builtin_setvar_helper(c, "SS7_CUG_INDICATOR", strp);

		if (p->cug_interlock_ni)
			pbx_builtin_setvar_helper(c, "SS7_CUG_INTERLOCK_NI", p->cug_interlock_ni);

		p->cug_indicator = ISUP_CUG_NON;
	}

	ast_mutex_lock(&p->lock);

	/* STARTPBX !!! */
	if (ast_pbx_start(c)) {
		ast_log(LOG_WARNING, "Unable to start PBX on %s\n", c->name);
		c->hangupcause = AST_CAUSE_SWITCH_CONGESTION; /* The ast_hangup() is dangerous here!!! */
		c->_softhangup |= AST_SOFTHANGUP_DEV;
	}

	ast_mutex_lock(&linkset->lock);
}

static void ss7_apply_plan_to_number(char *buf, size_t size, const struct dahdi_ss7 *ss7, const char *number, const unsigned nai)
{
	switch (nai) {
	case SS7_NAI_INTERNATIONAL:
		snprintf(buf, size, "%s%s", ss7->internationalprefix, number);
		break;
	case SS7_NAI_NATIONAL:
		snprintf(buf, size, "%s%s", ss7->nationalprefix, number);
		break;
	case SS7_NAI_SUBSCRIBER:
		snprintf(buf, size, "%s%s", ss7->subscriberprefix, number);
		break;
	case SS7_NAI_UNKNOWN:
		snprintf(buf, size, "%s%s", ss7->unknownprefix, number);
		break;
	case SS7_NAI_NETWORKROUTED:
		snprintf(buf, size, "%s%s", ss7->networkroutedprefix, number);
		break;
	default:
		snprintf(buf, size, "%s", number);
		break;
	}
}

static int ss7_pres_scr2cid_pres(char presentation_ind, char screening_ind)
{
    return ((presentation_ind & 0x3) << 5) | (screening_ind & 0x3);
}

static void ss7_process_connected(struct dahdi_pvt *p, char *connected_num, unsigned char connected_nai,
		unsigned char connected_presentation_ind, unsigned char connected_screening_ind) {
	struct ast_channel *c;
	char buf[AST_MAX_EXTENSION];

	if (!p->owner)
		return;

	c = p->owner;
	if (!ast_strlen_zero(connected_num)) {
		ss7_apply_plan_to_number(buf, sizeof(buf), p->ss7, connected_num, connected_nai);
		pbx_builtin_setvar_helper(c, "SS7_CONNECTED_NUMBER", buf);
	}

	pbx_builtin_setvar_helper(c, "SS7_CONNECTED_PRES",
			ast_named_caller_presentation(ss7_pres_scr2cid_pres(connected_presentation_ind, connected_screening_ind)));

}

static void *ss7_linkset(void *data)
{
	int res, i;
	struct timeval *next = NULL, tv;
	struct dahdi_ss7 *linkset = (struct dahdi_ss7 *) data;
	struct ss7 *ss7 = linkset->ss7;
	ss7_event *e = NULL;
	struct dahdi_pvt *p_cur, *p = NULL; /* just shut up gcc 4.1 */
	int chanpos;
	struct pollfd pollers[NUM_DCHANS];
	int cic;
	unsigned int dpc;
	unsigned char mb_state[255];
	int nextms = 0;

	ss7_start(ss7);

	while(1) {
		ast_mutex_lock(&linkset->lock);
		if ((next = ss7_schedule_next(ss7))) {
			tv = ast_tvnow();
			tv.tv_sec = next->tv_sec - tv.tv_sec;
			tv.tv_usec = next->tv_usec - tv.tv_usec;
			if (tv.tv_usec < 0) {
				tv.tv_usec += 1000000;
				tv.tv_sec -= 1;
			}
			if (tv.tv_sec < 0) {
				tv.tv_sec = 0;
				tv.tv_usec = 0;
			}
			nextms = tv.tv_sec * 1000;
			nextms += tv.tv_usec / 1000;
		} else
			nextms = -1;

		ast_mutex_unlock(&linkset->lock);

		for (i = 0; i < linkset->numsigchans; i++) {
			pollers[i].fd = linkset->fds[i];
			pollers[i].events = ss7_pollflags(ss7, linkset->fds[i]);
			pollers[i].revents = 0;
		}

		res = poll(pollers, linkset->numsigchans, nextms);
		if ((res < 0) && (errno != EINTR)) {
			ast_log(LOG_ERROR, "poll(%s)\n", strerror(errno));
		} else if (!res) {
			ast_mutex_lock(&linkset->lock);
			ss7_schedule_run(ss7);
			ast_mutex_unlock(&linkset->lock);
			/* continue; */
		}

		ast_mutex_lock(&linkset->lock);
		for (i = 0; i < linkset->numsigchans; i++) {
			if (pollers[i].revents & POLLPRI) {
				int x;
				if (ioctl(pollers[i].fd, DAHDI_GETEVENT, &x)) {
					ast_log(LOG_ERROR, "Error in exception retrieval!\n");
				}
				switch (x) {
				case DAHDI_EVENT_OVERRUN:
					ast_debug(1, "Overrun detected!\n");
					break;
				case DAHDI_EVENT_BADFCS:
					ast_debug(1, "Bad FCS\n");
					break;
				case DAHDI_EVENT_ABORT:
					ast_debug(1, "HDLC Abort\n");
					break;
				case DAHDI_EVENT_ALARM:
					ast_log(LOG_ERROR, "Alarm on link!\n");
					linkset->linkstate[i] |= (LINKSTATE_DOWN | LINKSTATE_INALARM);
					linkset->linkstate[i] &= ~LINKSTATE_UP;
					ss7_link_alarm(ss7, pollers[i].fd);
					break;
				case DAHDI_EVENT_NOALARM:
					ast_log(LOG_ERROR, "Alarm cleared on link\n");
					linkset->linkstate[i] &= ~(LINKSTATE_INALARM | LINKSTATE_DOWN);
					linkset->linkstate[i] |= LINKSTATE_STARTING;
					ss7_link_noalarm(ss7, pollers[i].fd);
					break;
				default:
					ast_log(LOG_ERROR, "Got exception %d!\n", x);
					break;
				}
			}

			if (pollers[i].revents & POLLIN) {
				ast_mutex_lock(&linkset->lock);
				res = ss7_read(ss7, pollers[i].fd);
				ast_mutex_unlock(&linkset->lock);
			}

			if (pollers[i].revents & POLLOUT) {
				ast_mutex_lock(&linkset->lock);
				res = ss7_write(ss7, pollers[i].fd);
				ast_mutex_unlock(&linkset->lock);
				if (res < 0) {
					ast_debug(1, "Error in write %s\n", strerror(errno));
				}
			}
		}

		while ((e = ss7_check_event(ss7))) {
			switch (e->e) {
			case SS7_EVENT_UP:
				if (linkset->state != LINKSET_STATE_UP) {
					ast_verbose("--- SS7 Up ---\n");
					ss7_reset_linkset(linkset);
				}
				linkset->state = LINKSET_STATE_UP;
				break;
			case SS7_EVENT_DOWN:
				ast_verbose("--- SS7 Down ---\n");
				linkset->state = LINKSET_STATE_DOWN;
				for (i = 0; i < linkset->numchans; i++) {
					struct dahdi_pvt *p = linkset->pvts[i];
					if (p) {
						p->inservice = 0;
						if (linkset->flags & LINKSET_FLAG_INITIALHWBLO)
							p->remotelyblocked |= SS7_BLOCKED_HARDWARE;
					}
				}
				break;
			case MTP2_LINK_UP:
				ast_verbose("MTP2 link up (SLC %d)\n", e->gen.data);
				break;
			case MTP2_LINK_DOWN:
				ast_log(LOG_WARNING, "MTP2 link down (SLC %d)\n", e->gen.data);
				break;
			case ISUP_EVENT_CPG:
				chanpos = ss7_find_cic(linkset, e->cpg.cic, e->cpg.opc);
				if (chanpos < 0) { /* Never will be true */
					ast_log(LOG_WARNING, "CPG on unconfigured CIC %d PC %d\n", e->cpg.cic, e->cpg.opc);
					isup_free_call(ss7, e->cpg.call);
					break;
				}
				p = linkset->pvts[chanpos];
				ast_mutex_lock(&p->lock);

				switch (e->cpg.event) {
				case CPG_EVENT_ALERTING:
					p->alerting = 1;
					p->subs[SUB_REAL].needringing = 1;
					break;
				case CPG_EVENT_PROGRESS:
				case CPG_EVENT_INBANDINFO:
					{
						struct ast_frame f = { AST_FRAME_CONTROL, AST_CONTROL_PROGRESS, };
						ast_debug(1, "Queuing frame PROGRESS on CIC %d\n", p->cic);
						dahdi_queue_frame(p, &f, linkset);
						p->progress = 1;
						p->dialing = 0;
						if (p->dsp && p->dsp_features) {
							ast_dsp_set_features(p->dsp, p->dsp_features);
							p->dsp_features = 0;
						}
					}
					break;
				default:
					ast_debug(1, "Do not handle CPG with event type 0x%x\n", e->cpg.event);
				}
				ast_mutex_unlock(&p->lock);
				break;
			case ISUP_EVENT_RSC:
				ast_verbose("Resetting CIC %d\n", e->rsc.cic);
				chanpos = ss7_find_cic(linkset, e->rsc.cic, e->rsc.opc);
				if (chanpos < 0) {
					ast_log(LOG_WARNING, "RSC on unconfigured CIC %d PC %d\n", e->rsc.cic, e->rsc.opc);
					isup_free_call(ss7, e->rsc.call);
					break;
				}
				p = linkset->pvts[chanpos];
				ast_mutex_lock(&p->lock);
				p->ss7call = e->rsc.call;
				p->inservice = 1;
				p->remotelyblocked = 0;
				dpc = p->dpc;
				if (p->locallyblocked == SS7_BLOCKED_MAINTENANCE)
					isup_blo(ss7, e->rsc.call);
				else if (p->locallyblocked == SS7_BLOCKED_HARDWARE)
					p->locallyblocked = 0;

				isup_set_call_dpc(e->rsc.call, dpc);

				if (p->owner) {
					p->owner->_softhangup |= AST_SOFTHANGUP_DEV;
					p->do_hangup = SS7_HANGUP_SEND_RLC;
					if (e->rsc.got_sent_msg != ISUP_SENT_IAM) {
						/* Q.784 6.2.3 */
						p->owner->hangupcause = AST_CAUSE_NORMAL_CLEARING;
					} else {
						p->owner->hangupcause = SS7_CAUSE_TRY_AGAIN;
					}
				} else {
					isup_rlc(ss7, e->rsc.call);
					p->ss7call = isup_free_call_if_clear(ss7, e->rsc.call);
				}
				/* End the loopback if we have one */
				dahdi_loopback(p, 0);
				ast_mutex_unlock(&p->lock);
				break;
			case ISUP_EVENT_GRS:
				if(!ss7_find_cic_range(linkset, e->grs.startcic, e->grs.endcic, e->grs.opc)) {
					ast_log(LOG_WARNING, "GRS on unconfigured range CIC %d - %d PC %d\n", e->grs.startcic, e->grs.endcic, e->grs.opc);
					chanpos = ss7_find_cic(linkset, e->gra.startcic, e->gra.opc);
					if(chanpos > -1) {
						p = linkset->pvts[chanpos];
						ast_mutex_lock(&p->lock);
						p->ss7call = isup_free_call_if_clear(ss7, e->grs.call);
						ast_mutex_unlock(&p->lock);
					} else
						isup_free_call(ss7, e->grs.call);
					break;
				}

				chanpos = ss7_find_cic(linkset, e->gra.startcic, e->gra.opc);
				p = linkset->pvts[chanpos];
				ast_mutex_lock(&p->lock);
				p->ss7call = e->gra.call;

				ss7_block_cics(linkset, e->grs.startcic, e->grs.endcic, e->grs.opc, NULL, 0, 1,  SS7_BLOCKED_HARDWARE);

				for (i = 0; i <= e->grs.endcic - p->cic; i++)	{
					chanpos = ss7_find_cic(p->ss7, i + p->cic, p->dpc);
					p_cur = p->ss7->pvts[chanpos];
					if(p != p_cur)
						ast_mutex_lock(&p_cur->lock);

					if (p_cur->locallyblocked & SS7_BLOCKED_MAINTENANCE) {
						mb_state[i] = 1;
					} else
						mb_state[i] = 0;

					if(p_cur->owner) {
						p_cur->owner->_softhangup |= AST_SOFTHANGUP_DEV;
						if(p_cur->owner->_state == AST_STATE_DIALING && !linkset->pvts[i]->proceeding)
							p_cur->owner->hangupcause = SS7_CAUSE_TRY_AGAIN;
						else
							p_cur->owner->hangupcause = AST_CAUSE_NORMAL_CLEARING;
						p_cur->do_hangup = SS7_HANGUP_FREE_CALL;
					} else if (p->ss7call && p != p_cur) { /* clear any session */
						isup_free_call(ss7, p_cur->ss7call);
						p_cur->ss7call = NULL;
					}

					p_cur->inservice = 1;

					if(p != p_cur)
						ast_mutex_unlock(&p_cur->lock);
				}

				isup_gra(ss7, p->ss7call, e->grs.endcic, mb_state);
				if(!p->owner)
					p->ss7call = isup_free_call_if_clear(ss7, p->ss7call);
				ast_mutex_unlock(&p->lock);
				break;
			case ISUP_EVENT_CQM:
				ast_debug(1, "Got Circuit group query message from CICs %d to %d\n", e->cqm.startcic, e->cqm.endcic);
				ss7_handle_cqm(linkset, e->cqm.startcic, e->cqm.endcic, e->cqm.opc);
				chanpos = ss7_find_cic(linkset, e->iam.cic, e->iam.opc);
				if (chanpos > -1) {
					p = linkset->pvts[chanpos];
					ast_mutex_lock(&p->lock);
					if (!p->owner)
						p->ss7call = isup_free_call_if_clear(ss7, e->cqm.call);
					ast_mutex_unlock(&p->lock);
				}
				break;

			case ISUP_EVENT_GRA:
				if(!ss7_find_cic_range(linkset, e->gra.startcic, e->gra.endcic, e->gra.opc)) { /* Never will be true */
					ast_log(LOG_WARNING, "GRA on unconfigured range CIC %d - %d PC %d\n", e->gra.startcic, e->gra.endcic, e->gra.opc);
					isup_free_call(ss7, e->gra.call);
					break;
				}
				chanpos = ss7_find_cic(linkset, e->gra.startcic, e->gra.opc);
				p = linkset->pvts[chanpos];
				ast_mutex_lock(&p->lock);
				p->ss7call = e->gra.call;

				ast_verbose("Got reset acknowledgement from CIC %d to %d DPC: %d\n", e->gra.startcic, e->gra.endcic, e->gra.opc);
				ss7_inservice(linkset, e->gra.startcic, e->gra.endcic, e->gra.opc);
				ss7_block_cics(linkset, e->gra.startcic, e->gra.endcic, e->gra.opc, e->gra.status,
					1, 1, SS7_BLOCKED_MAINTENANCE);

				p->ss7call = isup_free_call_if_clear(ss7, p->ss7call); /* we may sent a CDB with GRS! */
				ast_mutex_unlock(&p->lock);
				break;
			case ISUP_EVENT_SAM:
				chanpos = ss7_find_cic(linkset, e->sam.cic, e->sam.opc);
				if(chanpos < 0) {
					ast_log(LOG_WARNING, "SAM on unconfigured CIC %d PC %d\n", e->sam.cic, e->sam.opc);
					isup_free_call(ss7, e->sam.call);
					break;
				}
				p = linkset->pvts[chanpos];
				ast_mutex_lock(&p->lock);
				if(p->owner) {
					ast_log(LOG_WARNING, "SAM on CIC %d PC %d already have call\n", e->sam.cic, e->sam.opc);
					ast_mutex_unlock(&p->lock);
					break;
				}
				p->called_complete = 0;
				if (!ast_strlen_zero(e->sam.called_party_num)) {
					char *st;
					strncat(p->exten, e->sam.called_party_num, sizeof(p->exten) - strlen(p->exten));
					st = strchr(p->exten, '#');
					if (st) {
						*st = '\0';
						p->called_complete = 1;
					}
				} else
					p->exten[0] = '\0';
				goto ss7_start_switch;
			case ISUP_EVENT_IAM:
 				ast_debug(1, "Got IAM for CIC %d and called number %s, calling number %s\n", e->iam.cic, e->iam.called_party_num, e->iam.calling_party_num);
				chanpos = ss7_find_cic(linkset, e->iam.cic, e->iam.opc);
				if (chanpos < 0) {
					ast_log(LOG_WARNING, "IAM on unconfigured CIC %d PC %d\n", e->iam.cic, e->iam.opc);
					isup_free_call(ss7, e->iam.call);
					break;
				}
				p = linkset->pvts[chanpos];
				ast_mutex_lock(&p->lock);
				p->ss7call = e->iam.call;
				if (p->locallyblocked) {
					isup_clear_callflags(ss7, p->ss7call, ISUP_GOT_IAM);
					p->ss7call = isup_free_call_if_clear(ss7, p->ss7call);
					ast_mutex_unlock(&p->lock);
					ast_log(LOG_WARNING, "Got IAM on locally blocked CIC %d DPC %d, ignore\n", e->iam.cic, e->iam.opc);
					break;
				}
				if (p->remotelyblocked) {
					ast_log(LOG_NOTICE, "Got IAM on remotely blocked CIC %d DPC %d remove blocking\n", e->iam.cic, e->iam.opc);
					p->remotelyblocked = 0;
					p->inservice = 1;
				}
				if (p->owner) {
					ast_mutex_unlock(&p->lock);
					ast_log(LOG_WARNING, "Ring requested on CIC %d already in use!\n", e->iam.cic);
					break;
				}

				dpc = p->dpc;
				p->ss7call = e->iam.call;
				isup_set_call_dpc(p->ss7call, dpc);

				if ((p->use_callerid) && (!ast_strlen_zero(e->iam.calling_party_num))) {
					ss7_apply_plan_to_number(p->cid_num, sizeof(p->cid_num), linkset, e->iam.calling_party_num, e->iam.calling_nai);
					p->callingpres = ss7_pres_scr2cid_pres(e->iam.presentation_ind, e->iam.screening_ind);
				} else {
					p->cid_num[0] = 0;
					if (e->iam.presentation_ind)
						p->callingpres = ss7_pres_scr2cid_pres(e->iam.presentation_ind, e->iam.screening_ind);
				}

				p->called_complete = 0;
				if (p->immediate) {
					p->exten[0] = 's';
					p->exten[1] = '\0';
				} else if (!ast_strlen_zero(e->iam.called_party_num)) {
					char *st;
					ss7_apply_plan_to_number(p->exten, sizeof(p->exten), linkset, e->iam.called_party_num, e->iam.called_nai);
					st = strchr(p->exten, '#');
					if (st) {
						*st = '\0';
						p->called_complete = 1;
					}
				} else
					p->exten[0] = '\0';

				p->cid_ani[0] = '\0';
				if ((p->use_callerid) && (!ast_strlen_zero(e->iam.generic_name)))
					ast_copy_string(p->cid_name, e->iam.generic_name, sizeof(p->cid_name));
				else
					p->cid_name[0] = '\0';

				p->cid_ani2 = e->iam.oli_ani2;
				p->cid_ton = 0;
				ast_copy_string(p->charge_number, e->iam.charge_number, sizeof(p->charge_number));
				ast_copy_string(p->gen_add_number, e->iam.gen_add_number, sizeof(p->gen_add_number));
				p->gen_add_type = e->iam.gen_add_type;
				p->gen_add_nai = e->iam.gen_add_nai;
				p->gen_add_pres_ind = e->iam.gen_add_pres_ind;
				p->gen_add_num_plan = e->iam.gen_add_num_plan;
				ast_copy_string(p->gen_dig_number, e->iam.gen_dig_number, sizeof(p->gen_dig_number));
				p->gen_dig_type = e->iam.gen_dig_type;
				p->gen_dig_scheme = e->iam.gen_dig_scheme;
				ast_copy_string(p->jip_number, e->iam.jip_number, sizeof(p->jip_number));
				if (!ast_strlen_zero(e->iam.orig_called_num))
					ss7_apply_plan_to_number(p->orig_called_num, sizeof(p->orig_called_num), linkset, e->iam.orig_called_num, e->iam.orig_called_nai);
				if(!ast_strlen_zero(e->iam.redirecting_num))
					ss7_apply_plan_to_number(p->redirecting_num, sizeof(p->redirecting_num), linkset, e->iam.redirecting_num, e->iam.redirecting_num_nai);
				ast_copy_string(p->generic_name, e->iam.generic_name, sizeof(p->generic_name));
				p->calling_party_cat = e->iam.calling_party_cat;
				p->redirect_counter = e->iam.redirect_counter;
				p->redirect_info = e->iam.redirect_info;
				p->redirect_info_ind = e->iam.redirect_info_ind;
				p->redirect_info_orig_reas = e->iam.redirect_info_orig_reas;
				p->redirect_info_counter = e->iam.redirect_info_counter;
				p->redirect_info_reas = e->iam.redirect_info_reas;
				p->cug_indicator = e->iam.cug_indicator;
				p->cug_interlock_code = e->iam.cug_interlock_code;
				ast_copy_string(p->cug_interlock_ni, e->iam.cug_interlock_ni, sizeof(p->cug_interlock_ni));

				if (e->iam.cot_check_required)
						dahdi_loopback(p, 1);

				p->echocontrol_ind = e->iam.echocontrol_ind;
ss7_start_switch:
				if (option_verbose > 2)
					ast_verbose("SS7 exten: %s complete: %i\n", p->exten, p->called_complete);
				if (ast_exists_extension(NULL, p->context, p->exten, 1, p->cid_num) && (!ast_matchmore_extension(NULL, p->context, p->exten, 1, p->cid_num) || p->called_complete)) {
					p->called_complete = 1; /* If COT succesful start call! */
					/* Set DNID */
					strncpy(p->dnid, p->exten, sizeof(p->dnid));
					if ((e->e == ISUP_EVENT_IAM) ? !(e->iam.cot_check_required || e->iam.cot_performed_on_previous_cic) : (!(e->sam.cot_check_required || e->sam.cot_performed_on_previous_cic) || e->sam.cot_check_passed))
						ss7_start_call(p, linkset);
				} else if (ast_canmatch_extension(NULL, p->context, p->exten, 1, p->cid_num) && !p->called_complete) {
					isup_start_digittimeout(ss7, p->ss7call);
				} else if (!ast_matchmore_extension(NULL, p->context, p->exten, 1, p->cid_num) || p->called_complete) {
					ast_debug(1, "Call on CIC for unconfigured extension %s\n", p->exten);
					isup_rel(ss7, (e->e == ISUP_EVENT_IAM) ? e->iam.call : e->sam.call, AST_CAUSE_UNALLOCATED);
				}
				ast_mutex_unlock(&p->lock);

				if (e->e == ISUP_EVENT_IAM && e->iam.cot_performed_on_previous_cic) {
					chanpos = ss7_find_cic(linkset, (e->iam.cic - 1), e->iam.opc);
					if (chanpos < 0) {
						/* some stupid switch do this */
						ast_verbose("COT request on previous non exists CIC %d in IAM PC %d\n", (e->iam.cic - 1), e->iam.opc);
						break;
					}
					ast_verbose("COT request on previous CIC %d in IAM PC %d\n", (e->iam.cic - 1), e->iam.opc);
					p = linkset->pvts[chanpos];
					ast_mutex_lock(&p->lock);
					if (!p->ss7call) {
						p->inservice = 0; /* to prevent to use this circuit */
						dahdi_loopback(p, 1);
					} /* If already have a call don't loop */
					ast_mutex_unlock(&p->lock);
				}
				break;
			case ISUP_EVENT_DIGITTIMEOUT:
				chanpos = ss7_find_cic(linkset, e->digittimeout.cic, e->digittimeout.opc);
				if (chanpos < 0) {
					ast_log(LOG_WARNING, "DIGITTIMEOUT on unconfigured CIC %d PC %d\n", e->digittimeout.cic, e->digittimeout.opc);
					isup_free_call(ss7, e->digittimeout.call);
					break;
				}
				p = linkset->pvts[chanpos];
				ast_debug(1, "Digittimeout on CIC: %d PC: %d\n", e->digittimeout.cic, e->digittimeout.opc);
				ast_mutex_lock(&p->lock);
				p->called_complete = 1; /* If COT succesful start call! */
				strncpy(p->dnid, p->exten, sizeof(p->dnid));
				if (!(e->digittimeout.cot_check_required || e->digittimeout.cot_performed_on_previous_cic) || e->digittimeout.cot_check_passed)
					ss7_start_call(p, linkset);
				ast_mutex_unlock(&p->lock);
				break;
			case ISUP_EVENT_COT:
				if (e->cot.cot_performed_on_previous_cic) {
					chanpos = ss7_find_cic(linkset, (e->cot.cic - 1), e->cot.opc);
					/* some stupid switches do this!!! */
					if (chanpos > 0) {
						p = linkset->pvts[chanpos];
						ast_mutex_lock(&p->lock);
						p->inservice = 1;
						dahdi_loopback(p, 0);
						ast_mutex_unlock(&p->lock);
						ast_verbose("Loop turned off on CIC: %d PC: %d\n",  (e->cot.cic - 1), e->cot.opc);
					}
				}

				chanpos = ss7_find_cic(linkset, e->cot.cic, e->cot.opc);
				if (chanpos < 0) { /* Never will be true */
					ast_log(LOG_WARNING, "COT on unconfigured CIC %d PC %d\n", e->cot.cic, e->cot.opc);
					isup_free_call(ss7, e->cot.call);
					break;
				}
				p = linkset->pvts[chanpos];

				ast_mutex_lock(&p->lock);
				p->ss7call = e->cot.call;

				if (p->loopedback) {
					dahdi_loopback(p, 0);
					ast_verbose("Loop turned off on CIC: %d PC: %d\n",  e->cot.cic, e->cot.opc);
				}

				/* Don't start call if we didn't get IAM or COT failed! */
				if ((e->cot.got_sent_msg & ISUP_GOT_IAM) && e->cot.passed && p->called_complete)
					ss7_start_call(p, linkset);

				p->ss7call = isup_free_call_if_clear(ss7, p->ss7call);
				ast_mutex_unlock(&p->lock);
				break;
			case ISUP_EVENT_CCR:
				ast_debug(1, "Got CCR request on CIC %d\n", e->ccr.cic);
				chanpos = ss7_find_cic(linkset, e->ccr.cic, e->ccr.opc);
				if (chanpos < 0) {
					ast_log(LOG_WARNING, "CCR on unconfigured CIC %d PC %d\n", e->ccr.cic, e->ccr.opc);
					isup_free_call(ss7, e->ccr.call);
					break;
				}

				p = linkset->pvts[chanpos];

				ast_mutex_lock(&p->lock);
				p->ss7call = e->ccr.call;
				dahdi_loopback(p, 1);
				ast_mutex_unlock(&p->lock);

				isup_lpa(linkset->ss7, e->ccr.cic, p->dpc);
				break;
			case ISUP_EVENT_CVT:
				ast_debug(1, "Got CVT request on CIC %d\n", e->cvt.cic);
				chanpos = ss7_find_cic(linkset, e->cvt.cic, e->cvt.opc);
				if (chanpos < 0) {
					ast_log(LOG_WARNING, "CVT on unconfigured CIC %d PC %d\n", e->cvt.cic, e->cvt.opc);
					isup_free_call(ss7, e->cvt.call);
					break;
				}

				p = linkset->pvts[chanpos];

				ast_mutex_lock(&p->lock);
				p->ss7call = e->cvt.call;
				dahdi_loopback(p, 1);
				if (!p->owner)
					p->ss7call = isup_free_call_if_clear(ss7, e->cvt.call);
				ast_mutex_unlock(&p->lock);

				isup_cvr(linkset->ss7, e->cvt.cic, p->dpc);

				break;
			case ISUP_EVENT_REL:
				chanpos = ss7_find_cic(linkset, e->rel.cic, e->rel.opc);
				if (chanpos < 0) {
					ast_log(LOG_WARNING, "REL on unconfigured CIC %d PC %d\n", e->rel.cic, e->rel.opc);
					isup_free_call(ss7, e->rel.call);
					break;
				}
				p = linkset->pvts[chanpos];
				ast_mutex_lock(&p->lock);
				p->ss7call = e->rel.call;
				if (p->owner) {
					p->owner->hangupcause = e->rel.cause;
					p->owner->_softhangup |= AST_SOFTHANGUP_DEV;
					p->do_hangup = SS7_HANGUP_SEND_RLC;
					/* End the loopback if we have one */
					dahdi_loopback(p, 0);
				} else {
					ast_verbose("REL on CIC %d DPC %d without owner!\n", p->cic, p->dpc);
					isup_rlc(ss7, p->ss7call);
					p->ss7call = isup_free_call_if_clear(ss7, p->ss7call);
				}
				/* End the loopback if we have one */
				dahdi_loopback(p, 0);

				/* the rel is not complete here!!! */
				ast_mutex_unlock(&p->lock);
				break;
			case ISUP_EVENT_ACM:
				chanpos = ss7_find_cic(linkset, e->acm.cic, e->acm.opc);
				if (chanpos < 0) { /* Never will be true */
					ast_log(LOG_WARNING, "ACM on unconfigured CIC %d PC: %d\n", e->acm.cic, e->acm.opc);
					isup_free_call(ss7, e->acm.call);
					break;
				} else {
					p = linkset->pvts[chanpos];
					ast_mutex_lock(&p->lock);
					p->ss7call = e->acm.call;

					struct ast_frame f = { AST_FRAME_CONTROL, AST_CONTROL_PROCEEDING, };
					ast_debug(1, "Queueing frame from SS7_EVENT_ACM on CIC %d\n", p->cic);

					if (e->acm.call_ref_ident > 0) {
						p->rlt = 1; /* Setting it but not using it here*/
					}
					dahdi_queue_frame(p, &f, linkset);
					p->proceeding = 1;
					p->dialing = 0;
					/* Send alerting if subscriber is free */
					if (e->acm.called_party_status_ind == 1) {
						p->alerting = 1;
						p->subs[SUB_REAL].needringing = 1;
					}
					ast_mutex_unlock(&p->lock);
				}
				break;
			case ISUP_EVENT_CGB:
 				chanpos = ss7_find_cic(linkset, e->cgb.startcic, e->cgb.opc);
 				if (chanpos < 0) {
					isup_free_call(ss7, e->cgb.call);
 					ast_log(LOG_WARNING, "CGB on unconfigured CIC %d PC %d\n", e->cgb.startcic, e->cgb.opc);
 					break;
 				}
 				p = linkset->pvts[chanpos];
				ast_mutex_lock(&p->lock);
				p->ss7call = e->cgb.call;

				ss7_check_range(linkset, e->cgb.startcic, e->cgb.endcic, e->cgb.opc, e->cgb.status);

				ss7_block_cics(linkset, e->cgb.startcic, e->cgb.endcic, e->cgb.opc, e->cgb.status, 1, 1,
					(e->cgb.type) ? SS7_BLOCKED_HARDWARE : SS7_BLOCKED_MAINTENANCE);
 				isup_cgba(linkset->ss7, p->ss7call, e->cgb.endcic, e->cgb.status);
				if(!p->owner)
					p->ss7call = isup_free_call_if_clear(ss7, e->cgb.call);
				ast_mutex_unlock(&p->lock);
				break;
			case ISUP_EVENT_CGU:
 				chanpos = ss7_find_cic(linkset, e->cgu.startcic, e->cgu.opc);
 				if (chanpos < 0) {
					isup_free_call(ss7, e->cgu.call);
 					ast_log(LOG_WARNING, "CGU on unconfigured CIC %d PC %d\n", e->cgu.startcic, e->cgu.opc);
 					break;
 				}
 				p = linkset->pvts[chanpos];
				ast_mutex_lock(&p->lock);
				p->ss7call = e->cgu.call;

				ss7_check_range(linkset, e->cgu.startcic, e->cgu.endcic, e->cgu.opc, e->cgu.status);

				ss7_block_cics(linkset, e->cgu.startcic, e->cgu.endcic, e->cgu.opc, e->cgu.status, 0, 1,
					e->cgu.type ? SS7_BLOCKED_HARDWARE : SS7_BLOCKED_MAINTENANCE);
 				isup_cgua(linkset->ss7, p->ss7call, e->cgu.endcic, e->cgu.status);
				if(!p->owner)
					p->ss7call = isup_free_call_if_clear(ss7, e->cgu.call);
				ast_mutex_unlock(&p->lock);
				break;
			case ISUP_EVENT_UCIC:
				chanpos = ss7_find_cic(linkset, e->ucic.cic, e->ucic.opc);
				if (chanpos < 0) {
					ast_log(LOG_WARNING, "UCIC on unconfigured CIC %d PC %d\n", e->ucic.cic, e->ucic.opc);
					isup_free_call(ss7, e->ucic.call);
					break;
				}
				p = linkset->pvts[chanpos];
				ast_debug(1, "Unequiped Circuit Id Code on CIC %d\n", e->ucic.cic);
				ast_mutex_lock(&p->lock);
				p->ss7call = e->ucic.call;
				p->remotelyblocked = SS7_BLOCKED_MAINTENANCE;
				p->inservice = 0;
				p->ss7call = NULL;
				isup_free_call(ss7, e->ucic.call);
				if (p->owner)
					p->owner->_softhangup |= AST_SOFTHANGUP_DEV;
				ast_mutex_unlock(&p->lock);			/* doesn't require a SS7 acknowledgement */
				break;
			case ISUP_EVENT_BLO:
				chanpos = ss7_find_cic(linkset, e->blo.cic, e->blo.opc);
				if (chanpos < 0) {
					ast_log(LOG_WARNING, "BLO on unconfigured CIC %d PC %d\n", e->blo.cic, e->blo.opc);
					isup_free_call(ss7, e->blo.call);
					break;
				}
				p = linkset->pvts[chanpos];
				p->ss7call = e->blo.call;
				ast_debug(1, "Blocking CIC %d\n", e->blo.cic);
				ast_mutex_lock(&p->lock);
				p->remotelyblocked |= SS7_BLOCKED_MAINTENANCE;
				isup_bla(linkset->ss7, e->blo.call);
				if (!p->owner)
					p->ss7call = isup_free_call_if_clear(ss7, e->blo.call);
				else if (e->blo.got_sent_msg == ISUP_SENT_IAM) {
					/* Q.784 6.2.2 */
					p->owner->hangupcause = SS7_CAUSE_TRY_AGAIN;
					p->owner->_softhangup |= AST_SOFTHANGUP_DEV;
				}
				ast_mutex_unlock(&p->lock);
				break;
			case ISUP_EVENT_BLA:
				chanpos = ss7_find_cic(linkset, e->bla.cic, e->bla.opc);
				if (chanpos < 0) { /* Never will be true */
					ast_log(LOG_WARNING, "BLA on unconfigured CIC %d PC %d\n", e->bla.cic, e->bla.opc);
					isup_free_call(ss7, e->bla.call);
					break;
				}
				p = linkset->pvts[chanpos];
				p->ss7call = e->bla.call;
				ast_mutex_lock(&p->lock);
				ast_debug(1, "Locally blocking CIC %d\n", e->bla.cic);
				p->locallyblocked |= SS7_BLOCKED_MAINTENANCE;
				if (!p->owner)
					p->ss7call = isup_free_call_if_clear(ss7, p->ss7call);
				ast_mutex_unlock(&p->lock);
				break;
			case ISUP_EVENT_UBL:
				chanpos = ss7_find_cic(linkset, e->ubl.cic, e->ubl.opc);
				if (chanpos < 0) {
					ast_log(LOG_WARNING, "UBL on unconfigured CIC %d PC %d\n", e->ubl.cic, e->ubl.opc);
					isup_free_call(ss7, e->ubl.call);
					break;
				}
				p = linkset->pvts[chanpos];
				ast_debug(1, "Remotely unblocking CIC %d PC %d\n", e->ubl.cic, e->ubl.opc);
				ast_mutex_lock(&p->lock);
				p->ss7call = e->ubl.call;
				p->remotelyblocked &= ~SS7_BLOCKED_MAINTENANCE;
				isup_uba(linkset->ss7, e->ubl.call);
				if (!p->owner)
					p->ss7call = isup_free_call_if_clear(ss7, p->ss7call);
				ast_mutex_unlock(&p->lock);
				break;
			case ISUP_EVENT_UBA:
				chanpos = ss7_find_cic(linkset, e->uba.cic, e->uba.opc);
				if (chanpos < 0) {
					ast_log(LOG_WARNING, "UBA on unconfigured CIC %d PC %d\n", e->uba.cic, e->uba.opc);
					isup_free_call(ss7, e->uba.call);
					break;
				}
				p = linkset->pvts[chanpos];
				ast_mutex_lock(&p->lock);
				p->ss7call = e->uba.call;
				ast_debug(1, "Locally unblocking CIC %d PC %d\n", e->uba.cic, e->uba.opc);
				p->locallyblocked &= ~SS7_BLOCKED_MAINTENANCE;
				if (!p->owner)
					p->ss7call = isup_free_call_if_clear(ss7, p->ss7call);
				ast_mutex_unlock(&p->lock);
				break;
			case ISUP_EVENT_CON:
			case ISUP_EVENT_ANM:
				if (e->e == ISUP_EVENT_CON) {
					cic = e->con.cic;
					if (e->con.connected_num)
						ss7_process_connected(p, e->con.connected_num, e->con.connected_nai,
								e->con.connected_presentation_ind, e->con.connected_screening_ind);
				} else {
					cic = e->anm.cic;
					if (e->anm.connected_num)
						ss7_process_connected(p, e->anm.connected_num, e->anm.connected_nai,
								e->anm.connected_presentation_ind, e->anm.connected_screening_ind);
				}

				chanpos = ss7_find_cic(linkset, cic, (e->e == ISUP_EVENT_ANM) ? e->anm.opc : e->con.opc);
				if (chanpos < 0) { /* Never will be true */
					ast_log(LOG_WARNING, "ANM/CON on unconfigured CIC %d PC %d\n", cic, (e->e == ISUP_EVENT_ANM) ? e->anm.opc : e->con.opc);
					isup_free_call(ss7, (e->e == ISUP_EVENT_ANM) ? e->anm.call : e->con.call);
					break;
				} else {
					p = linkset->pvts[chanpos];
					ast_mutex_lock(&p->lock);
					p->ss7call = (e->e == ISUP_EVENT_ANM) ?  e->anm.call : e->con.call;
					p->subs[SUB_REAL].needanswer = 1;
					if (p->dsp && p->dsp_features) {
						ast_dsp_set_features(p->dsp, p->dsp_features);
						p->dsp_features = 0;
					}
					if ((e->e == ISUP_EVENT_ANM) ? !e->anm.echocontrol_ind  : !e->con.echocontrol_ind || !(linkset->flags & LINKSET_FLAG_USEECHOCONTROL)) {
						dahdi_enable_ec(p);
						dahdi_train_ec(p);
					}
					ast_mutex_unlock(&p->lock);
				}
				break;
			case ISUP_EVENT_RLC:
				chanpos = ss7_find_cic(linkset, e->rlc.cic, e->rlc.opc);
				if (chanpos < 0) { /* Never will be true */
					ast_log(LOG_WARNING, "RLC on unconfigured CIC %d PC %d\n", e->rlc.cic, e->rlc.opc);
					isup_free_call(ss7, e->rlc.call);
					break;
				} else {
					p = linkset->pvts[chanpos];
					ast_mutex_lock(&p->lock);
					p->ss7call = e->rlc.call;
					if (e->rlc.got_sent_msg & (ISUP_SENT_RSC | ISUP_SENT_REL)) {
						dahdi_loopback(p, 0);
						if (e->rlc.got_sent_msg & ISUP_SENT_RSC)
							p->inservice = 1;
					}
					if (!p->owner)
						p->ss7call = isup_free_call_if_clear(ss7, e->rlc.call);
					else {
						p->owner->_softhangup |= AST_SOFTHANGUP_DEV;
						p->do_hangup = SS7_HANGUP_DO_NOTHING;
					}
					ast_mutex_unlock(&p->lock);
				}
				break;
			case ISUP_EVENT_FAA:
				chanpos = ss7_find_cic(linkset, e->faa.cic, e->faa.opc);
				if (chanpos < 0) {
					ast_log(LOG_WARNING, "FAA on unconfigured CIC %d PC %d\n", e->faa.cic, e->faa.opc);
					isup_free_call(ss7, e->faa.call);
					break;
				} else {
					p = linkset->pvts[chanpos];
					p->ss7call = e->faa.call;
					ast_debug(1, "FAA received on CIC %d\n", e->faa.cic);
					ast_mutex_lock(&p->lock);
					/*if (p->alreadyhungup)
						ast_log(LOG_NOTICE, "Received FAA and we haven't sent FAR.  Ignoring.\n");
					FIX IT !!! */
					p->ss7call = isup_free_call_if_clear(ss7, p->ss7call);
					ast_mutex_unlock(&p->lock);
				}
				break;
			case ISUP_EVENT_CGBA:
				chanpos = ss7_find_cic(linkset, e->cgba.startcic, e->cgba.opc);
				if (chanpos < 0) { /* Never will be true */
					ast_log(LOG_WARNING, "CGBA on unconfigured CIC %d PC %d\n", e->cgba.startcic, e->cgba.opc);
					isup_free_call(ss7, e->cgba.call);
					break;
				}
				p = linkset->pvts[chanpos];
				ast_mutex_lock(&p->lock);
				p->ss7call = e->cgba.call;

				ss7_block_cics(linkset, e->cgba.startcic, e->cgba.endcic, e->cgba.opc, e->cgba.status, 1, 0,
						e->cgba.type ? SS7_BLOCKED_HARDWARE : SS7_BLOCKED_MAINTENANCE);

				if(!p->owner)
					p->ss7call = isup_free_call_if_clear(ss7, p->ss7call);
				ast_mutex_unlock(&p->lock);
				break;
			case ISUP_EVENT_CGUA:
				chanpos = ss7_find_cic(linkset, e->cgua.startcic, e->cgua.opc);
				if (chanpos < 0) { /* Never will be true */
					ast_log(LOG_WARNING, "CGUA on unconfigured CIC %d PC %d\n", e->cgua.startcic, e->cgua.opc);
					isup_free_call(ss7, e->cgua.call);
					break;
				}
				p = linkset->pvts[chanpos];
				ast_mutex_lock(&p->lock);
				p->ss7call = e->cgua.call;

				ss7_block_cics(linkset, e->cgua.startcic, e->cgua.endcic, e->cgua.opc, e->cgua.status, 0, 0,
					e->cgba.type ? SS7_BLOCKED_HARDWARE : SS7_BLOCKED_MAINTENANCE);
				if(!p->owner)
					p->ss7call = isup_free_call_if_clear(ss7, p->ss7call);
				ast_mutex_unlock(&p->lock);
				break;
			case ISUP_EVENT_SUS:
			case ISUP_EVENT_RES:
				chanpos = ss7_find_cic(linkset, e->susres.cic, e->susres.opc);
				if (chanpos < 0) {
					ast_log(LOG_WARNING, "SUS/RES on unconfigured CIC %d PC %d\n", e->susres.cic, e->susres.opc);
					isup_free_call(ss7, e->susres.call);
					break;
				}
				p = linkset->pvts[chanpos];
				ast_mutex_lock(&p->lock);
				p->ss7call = e->susres.call;
				if(!p->owner)
					p->ss7call = isup_free_call_if_clear(ss7, p->ss7call);
				ast_mutex_unlock(&p->lock);
				break;
			default:
				ast_debug(1, "Unknown event %s\n", ss7_event2str(e->e));
				break;
			}
		}
		ast_mutex_unlock(&linkset->lock);
	}

	return 0;
}

static struct dahdi_pvt * ss7_find_pvt(struct ss7 *ss7, int cic, unsigned int dpc)
{
	int i, chanpos;
	struct dahdi_ss7 *winner = NULL;

	for (i = 0; i < NUM_SPANS; i++)
		if(linksets[i].ss7 == ss7) {
			winner = &linksets[i];
			break;
		}

	if (winner && (chanpos = ss7_find_cic(winner, cic, dpc)) > -1) {
		return winner->pvts[chanpos];
	} else
		return NULL;
}

static int dahdi_ss7_hangup(struct ss7 *ss7, int cic, unsigned int dpc, int cause, int do_hangup)
{
	struct dahdi_pvt *p;
	int res;

	if(!(p = ss7_find_pvt(ss7, cic, dpc))) {
		return SS7_CIC_NOT_EXISTS;
	}

	ast_mutex_lock(&p->lock);
	if (p->owner) {
		p->owner->hangupcause = cause;
		p->owner->_softhangup |= AST_SOFTHANGUP_DEV;
		p->do_hangup = do_hangup;
		res = SS7_CIC_USED;
	} else
		res = SS7_CIC_IDLE;
	ast_mutex_unlock(&p->lock);

	return res;
}

static void dahdi_ss7_call_null(struct ss7 *ss7, struct isup_call *c, int lock)
{
	int i;
	struct dahdi_ss7 *winner = NULL;

	for (i = 0; i < NUM_SPANS; i++)
		if(linksets[i].ss7 == ss7) {
			winner = &linksets[i];
			break;
		}

	if (winner)
		for (i = 0; i < winner->numchans; i++)
			if (winner->pvts[i] && (winner->pvts[i]->ss7call == c)) {
				if (lock)
					ast_mutex_lock(&winner->pvts[i]->lock);
				winner->pvts[i]->ss7call = NULL;
				if (winner->pvts[i]->owner) {
					winner->pvts[i]->owner->hangupcause = AST_CAUSE_NORMAL_TEMPORARY_FAILURE;
					winner->pvts[i]->owner->_softhangup |= AST_SOFTHANGUP_DEV;
				}
				if (lock)
					ast_mutex_unlock(&winner->pvts[i]->lock);
				ast_log(LOG_WARNING, "libss7 asked set to NULL ss7 call on CIC %d DPC %d\n", winner->pvts[i]->cic, winner->pvts[i]->dpc);
			}
}

static void dahdi_ss7_notinservice(struct ss7 *ss7, int cic, unsigned int dpc)
{
	struct dahdi_pvt *p;
	if(!(p = ss7_find_pvt(ss7, cic, dpc)))
		return;

	ast_mutex_lock(&p->lock);
	p->inservice = 0;
	ast_mutex_unlock(&p->lock);
}

static void dahdi_ss7_message(struct ss7 *ss7, char *s)
{
#if 0
	int i;

	for (i = 0; i < NUM_SPANS; i++)
		if (linksets[i].ss7 == ss7)
			break;

	ast_verbose("[%d] %s", i+1, s);
#else
if (option_verbose > 2)
	ast_verbose("%s", s);
#endif
}

static void dahdi_ss7_error(struct ss7 *ss7, char *s)
{
#if 0
	int i;

	for (i = 0; i < NUM_SPANS; i++)
		if (linksets[i].ss7 == ss7)
			break;

#else
	ast_log(LOG_ERROR, "%s", s);
#endif
}

#endif /* HAVE_SS7 */

#ifdef HAVE_PRI
static struct dahdi_pvt *pri_find_crv(struct dahdi_pri *pri, int crv)
{
	struct dahdi_pvt *p;
	p = pri->crvs;
	while (p) {
		if (p->channel == crv)
			return p;
		p = p->next;
	}
	return NULL;
}


static int pri_find_principle(struct dahdi_pri *pri, int channel)
{
	int x;
	int span = PRI_SPAN(channel);
	int spanfd;
	struct dahdi_params param;
	int principle = -1;
	int explicit = PRI_EXPLICIT(channel);
	channel = PRI_CHANNEL(channel);

	if (!explicit) {
		spanfd = pri_active_dchan_fd(pri);
		memset(&param, 0, sizeof(param));
		if (ioctl(spanfd, DAHDI_GET_PARAMS, &param))
			return -1;
		span = pris[param.spanno - 1].prilogicalspan;
	}

	for (x = 0; x < pri->numchans; x++) {
		if (pri->pvts[x] && (pri->pvts[x]->prioffset == channel) && (pri->pvts[x]->logicalspan == span)) {
			principle = x;
			break;
		}
	}

	return principle;
}

static int pri_fixup_principle(struct dahdi_pri *pri, int principle, q931_call *c)
{
	int x;
	struct dahdi_pvt *crv;
	if (!c) {
		if (principle < 0)
			return -1;
		return principle;
	}
	if ((principle > -1) &&
		(principle < pri->numchans) &&
		(pri->pvts[principle]) &&
		(pri->pvts[principle]->call == c))
		return principle;
	/* First, check for other bearers */
	for (x = 0; x < pri->numchans; x++) {
		if (!pri->pvts[x])
			continue;
		if (pri->pvts[x]->call == c) {
			/* Found our call */
			if (principle != x) {
				struct dahdi_pvt *new = pri->pvts[principle], *old = pri->pvts[x];

				ast_verb(3, "Moving call from channel %d to channel %d\n",
					 old->channel, new->channel);
				if (new->owner) {
					ast_log(LOG_WARNING, "Can't fix up channel from %d to %d because %d is already in use\n",
						old->channel, new->channel, new->channel);
					return -1;
				}
				/* Fix it all up now */
				new->owner = old->owner;
				old->owner = NULL;
				if (new->owner) {
					ast_string_field_build(new->owner, name,
							       "DAHDI/%d:%d-%d", pri->trunkgroup,
							       new->channel, 1);
					new->owner->tech_pvt = new;
					ast_channel_set_fd(new->owner, 0, new->subs[SUB_REAL].dfd);
					new->subs[SUB_REAL].owner = old->subs[SUB_REAL].owner;
					old->subs[SUB_REAL].owner = NULL;
				} else
					ast_log(LOG_WARNING, "Whoa, there's no  owner, and we're having to fix up channel %d to channel %d\n", old->channel, new->channel);
				new->call = old->call;
				old->call = NULL;

				/* Copy any DSP that may be present */
				new->dsp = old->dsp;
				new->dsp_features = old->dsp_features;
				old->dsp = NULL;
				old->dsp_features = 0;
			}
			return principle;
		}
	}
	/* Now check for a CRV with no bearer */
	crv = pri->crvs;
	while (crv) {
		if (crv->call == c) {
			/* This is our match...  Perform some basic checks */
			if (crv->bearer)
				ast_log(LOG_WARNING, "Trying to fix up call which already has a bearer which isn't the one we think it is\n");
			else if (pri->pvts[principle]->owner)
				ast_log(LOG_WARNING, "Tring to fix up a call to a bearer which already has an owner!\n");
			else {
				/* Looks good.  Drop the pseudo channel now, clear up the assignment, and
				   wakeup the potential sleeper */
				dahdi_close(crv->subs[SUB_REAL].dfd);
				pri->pvts[principle]->call = crv->call;
				pri_assign_bearer(crv, pri, pri->pvts[principle]);
				ast_debug(1, "Assigning bearer %d/%d to CRV %d:%d\n",
					pri->pvts[principle]->logicalspan, pri->pvts[principle]->prioffset,
					pri->trunkgroup, crv->channel);
				wakeup_sub(crv, SUB_REAL, pri);
			}
			return principle;
		}
		crv = crv->next;
	}
	ast_log(LOG_WARNING, "Call specified, but not found?\n");
	return -1;
}

static void *do_idle_thread(void *vchan)
{
	struct ast_channel *chan = vchan;
	struct dahdi_pvt *pvt = chan->tech_pvt;
	struct ast_frame *f;
	char ex[80];
	/* Wait up to 30 seconds for an answer */
	int newms, ms = 30000;
	ast_verb(3, "Initiating idle call on channel %s\n", chan->name);
	snprintf(ex, sizeof(ex), "%d/%s", pvt->channel, pvt->pri->idledial);
	if (ast_call(chan, ex, 0)) {
		ast_log(LOG_WARNING, "Idle dial failed on '%s' to '%s'\n", chan->name, ex);
		ast_hangup(chan);
		return NULL;
	}
	while ((newms = ast_waitfor(chan, ms)) > 0) {
		f = ast_read(chan);
		if (!f) {
			/* Got hangup */
			break;
		}
		if (f->frametype == AST_FRAME_CONTROL) {
			switch (f->subclass) {
			case AST_CONTROL_ANSWER:
				/* Launch the PBX */
				ast_copy_string(chan->exten, pvt->pri->idleext, sizeof(chan->exten));
				ast_copy_string(chan->context, pvt->pri->idlecontext, sizeof(chan->context));
				chan->priority = 1;
				ast_verb(4, "Idle channel '%s' answered, sending to %s@%s\n", chan->name, chan->exten, chan->context);
				ast_pbx_run(chan);
				/* It's already hungup, return immediately */
				return NULL;
			case AST_CONTROL_BUSY:
				ast_verb(4, "Idle channel '%s' busy, waiting...\n", chan->name);
				break;
			case AST_CONTROL_CONGESTION:
				ast_verb(4, "Idle channel '%s' congested, waiting...\n", chan->name);
				break;
			};
		}
		ast_frfree(f);
		ms = newms;
	}
	/* Hangup the channel since nothing happend */
	ast_hangup(chan);
	return NULL;
}

#ifndef PRI_RESTART
#error "Upgrade your libpri"
#endif
static void dahdi_pri_message(struct pri *pri, char *s)
{
	int x, y;
	int dchan = -1, span = -1;
	int dchancount = 0;

	if (pri) {
		for (x = 0; x < NUM_SPANS; x++) {
			for (y = 0; y < NUM_DCHANS; y++) {
				if (pris[x].dchans[y])
					dchancount++;

				if (pris[x].dchans[y] == pri)
					dchan = y;
			}
			if (dchan >= 0) {
				span = x;
				break;
			}
			dchancount = 0;
		}
		if (dchancount > 1 && (span > -1))
			ast_verbose("[Span %d D-Channel %d]%s", span, dchan, s);
		else
			ast_verbose("%s", s);
	} else
		ast_verbose("%s", s);

	ast_mutex_lock(&pridebugfdlock);

	if (pridebugfd >= 0)
		write(pridebugfd, s, strlen(s));

	ast_mutex_unlock(&pridebugfdlock);
}

static void dahdi_pri_error(struct pri *pri, char *s)
{
	int x, y;
	int dchan = -1, span = -1;
	int dchancount = 0;

	if (pri) {
		for (x = 0; x < NUM_SPANS; x++) {
			for (y = 0; y < NUM_DCHANS; y++) {
				if (pris[x].dchans[y])
					dchancount++;

				if (pris[x].dchans[y] == pri)
					dchan = y;
			}
			if (dchan >= 0) {
				span = x;
				break;
			}
			dchancount = 0;
		}
		if ((dchancount > 1) && (span > -1))
			ast_log(LOG_ERROR, "[Span %d D-Channel %d] PRI: %s", span, dchan, s);
		else
			ast_log(LOG_ERROR, "%s", s);
	} else
		ast_log(LOG_ERROR, "%s", s);

	ast_mutex_lock(&pridebugfdlock);

	if (pridebugfd >= 0)
		write(pridebugfd, s, strlen(s));

	ast_mutex_unlock(&pridebugfdlock);
}

static int pri_check_restart(struct dahdi_pri *pri)
{
	do {
		pri->resetpos++;
	} while ((pri->resetpos < pri->numchans) &&
		 (!pri->pvts[pri->resetpos] ||
		  pri->pvts[pri->resetpos]->call ||
		  pri->pvts[pri->resetpos]->resetting));
	if (pri->resetpos < pri->numchans) {
		/* Mark the channel as resetting and restart it */
		pri->pvts[pri->resetpos]->resetting = 1;
		pri_reset(pri->pri, PVT_TO_CHANNEL(pri->pvts[pri->resetpos]));
	} else {
		pri->resetting = 0;
		time(&pri->lastreset);
	}
	return 0;
}

static int pri_hangup_all(struct dahdi_pvt *p, struct dahdi_pri *pri)
{
	int x;
	int redo;
	ast_mutex_unlock(&pri->lock);
	ast_mutex_lock(&p->lock);
	do {
		redo = 0;
		for (x = 0; x < 3; x++) {
			while (p->subs[x].owner && ast_channel_trylock(p->subs[x].owner)) {
				redo++;
				DEADLOCK_AVOIDANCE(&p->lock);
			}
			if (p->subs[x].owner) {
				ast_queue_hangup(p->subs[x].owner);
				ast_channel_unlock(p->subs[x].owner);
			}
		}
	} while (redo);
	ast_mutex_unlock(&p->lock);
	ast_mutex_lock(&pri->lock);
	return 0;
}

static char * redirectingreason2str(int redirectingreason)
{
	switch (redirectingreason) {
	case 0:
		return "UNKNOWN";
	case 1:
		return "BUSY";
	case 2:
		return "NO_REPLY";
	case 0xF:
		return "UNCONDITIONAL";
	default:
		return "NOREDIRECT";
	}
}

static void apply_plan_to_number(char *buf, size_t size, const struct dahdi_pri *pri, const char *number, const int plan)
{
	if (pri->dialplan == -2) { /* autodetect the TON but leave the number untouched */
		snprintf(buf, size, "%s", number);
		return;
	}
	switch (plan) {
	case PRI_INTERNATIONAL_ISDN:		/* Q.931 dialplan == 0x11 international dialplan => prepend international prefix digits */
		snprintf(buf, size, "%s%s", pri->internationalprefix, number);
		break;
	case PRI_NATIONAL_ISDN:			/* Q.931 dialplan == 0x21 national dialplan => prepend national prefix digits */
		snprintf(buf, size, "%s%s", pri->nationalprefix, number);
		break;
	case PRI_LOCAL_ISDN:			/* Q.931 dialplan == 0x41 local dialplan => prepend local prefix digits */
		snprintf(buf, size, "%s%s", pri->localprefix, number);
		break;
	case PRI_PRIVATE:			/* Q.931 dialplan == 0x49 private dialplan => prepend private prefix digits */
		snprintf(buf, size, "%s%s", pri->privateprefix, number);
		break;
	case PRI_UNKNOWN:			/* Q.931 dialplan == 0x00 unknown dialplan => prepend unknown prefix digits */
		snprintf(buf, size, "%s%s", pri->unknownprefix, number);
		break;
	default:				/* other Q.931 dialplan => don't twiddle with callingnum */
		snprintf(buf, size, "%s", number);
		break;
	}
}


static void *pri_dchannel(void *vpri)
{
	struct dahdi_pri *pri = vpri;
	pri_event *e;
	struct pollfd fds[NUM_DCHANS];
	int res;
	int chanpos = 0;
	int x;
	int haveidles;
	int activeidles;
	int nextidle = -1;
	struct ast_channel *c;
	struct timeval tv, lowest, *next;
	struct timeval lastidle = ast_tvnow();
	int doidling=0;
	char *cc;
	char idlen[80];
	struct ast_channel *idle;
	pthread_t p;
	time_t t;
	int i, which=-1;
	int numdchans;
	int cause=0;
	struct dahdi_pvt *crv;
	pthread_t threadid;
	char ani2str[6];
	char plancallingnum[256];
	char plancallingani[256];
	char calledtonstr[10];

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	gettimeofday(&lastidle, NULL);
	if (!ast_strlen_zero(pri->idledial) && !ast_strlen_zero(pri->idleext)) {
		/* Need to do idle dialing, check to be sure though */
		cc = strchr(pri->idleext, '@');
		if (cc) {
			*cc = '\0';
			cc++;
			ast_copy_string(pri->idlecontext, cc, sizeof(pri->idlecontext));
#if 0
			/* Extensions may not be loaded yet */
			if (!ast_exists_extension(NULL, pri->idlecontext, pri->idleext, 1, NULL))
				ast_log(LOG_WARNING, "Extension '%s @ %s' does not exist\n", pri->idleext, pri->idlecontext);
			else
#endif
				doidling = 1;
		} else
			ast_log(LOG_WARNING, "Idle dial string '%s' lacks '@context'\n", pri->idleext);
	}
	for (;;) {
		for (i = 0; i < NUM_DCHANS; i++) {
			if (!pri->dchannels[i])
				break;
			fds[i].fd = pri->fds[i];
			fds[i].events = POLLIN | POLLPRI;
			fds[i].revents = 0;
		}
		numdchans = i;
		time(&t);
		ast_mutex_lock(&pri->lock);
		if (pri->switchtype != PRI_SWITCH_GR303_TMC && (pri->resetinterval > 0)) {
			if (pri->resetting && pri_is_up(pri)) {
				if (pri->resetpos < 0)
					pri_check_restart(pri);
			} else {
				if (!pri->resetting	&& (t - pri->lastreset) >= pri->resetinterval) {
					pri->resetting = 1;
					pri->resetpos = -1;
				}
			}
		}
		/* Look for any idle channels if appropriate */
		if (doidling && pri_is_up(pri)) {
			nextidle = -1;
			haveidles = 0;
			activeidles = 0;
			for (x = pri->numchans; x >= 0; x--) {
				if (pri->pvts[x] && !pri->pvts[x]->owner &&
				    !pri->pvts[x]->call) {
					if (haveidles < pri->minunused) {
						haveidles++;
					} else if (!pri->pvts[x]->resetting) {
						nextidle = x;
						break;
					}
				} else if (pri->pvts[x] && pri->pvts[x]->owner && pri->pvts[x]->isidlecall)
					activeidles++;
			}
			if (nextidle > -1) {
				if (ast_tvdiff_ms(ast_tvnow(), lastidle) > 1000) {
					/* Don't create a new idle call more than once per second */
					snprintf(idlen, sizeof(idlen), "%d/%s", pri->pvts[nextidle]->channel, pri->idledial);
					idle = dahdi_request("DAHDI", AST_FORMAT_ULAW, idlen, &cause);
					if (idle) {
						pri->pvts[nextidle]->isidlecall = 1;
						if (ast_pthread_create_background(&p, NULL, do_idle_thread, idle)) {
							ast_log(LOG_WARNING, "Unable to start new thread for idle channel '%s'\n", idle->name);
							dahdi_hangup(idle);
						}
					} else
						ast_log(LOG_WARNING, "Unable to request channel 'DAHDI/%s' for idle call\n", idlen);
					lastidle = ast_tvnow();
				}
			} else if ((haveidles < pri->minunused) &&
				   (activeidles > pri->minidle)) {
				/* Mark something for hangup if there is something
				   that can be hungup */
				for (x = pri->numchans; x >= 0; x--) {
					/* find a candidate channel */
					if (pri->pvts[x] && pri->pvts[x]->owner && pri->pvts[x]->isidlecall) {
						pri->pvts[x]->owner->_softhangup |= AST_SOFTHANGUP_DEV;
						haveidles++;
						/* Stop if we have enough idle channels or
						  can't spare any more active idle ones */
						if ((haveidles >= pri->minunused) ||
						    (activeidles <= pri->minidle))
							break;
					}
				}
			}
		}
		/* Start with reasonable max */
		lowest = ast_tv(60, 0);
		for (i = 0; i < NUM_DCHANS; i++) {
			/* Find lowest available d-channel */
			if (!pri->dchannels[i])
				break;
			if ((next = pri_schedule_next(pri->dchans[i]))) {
				/* We need relative time here */
				tv = ast_tvsub(*next, ast_tvnow());
				if (tv.tv_sec < 0) {
					tv = ast_tv(0,0);
				}
				if (doidling || pri->resetting) {
					if (tv.tv_sec > 1) {
						tv = ast_tv(1, 0);
					}
				} else {
					if (tv.tv_sec > 60) {
						tv = ast_tv(60, 0);
					}
				}
			} else if (doidling || pri->resetting) {
				/* Make sure we stop at least once per second if we're
				   monitoring idle channels */
				tv = ast_tv(1,0);
			} else {
				/* Don't poll for more than 60 seconds */
				tv = ast_tv(60, 0);
			}
			if (!i || ast_tvcmp(tv, lowest) < 0) {
				lowest = tv;
			}
		}
		ast_mutex_unlock(&pri->lock);

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		pthread_testcancel();
		e = NULL;
		res = poll(fds, numdchans, lowest.tv_sec * 1000 + lowest.tv_usec / 1000);
		pthread_testcancel();
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		ast_mutex_lock(&pri->lock);
		if (!res) {
			for (which = 0; which < NUM_DCHANS; which++) {
				if (!pri->dchans[which])
					break;
				/* Just a timeout, run the scheduler */
				e = pri_schedule_run(pri->dchans[which]);
				if (e)
					break;
			}
		} else if (res > -1) {
			for (which = 0; which < NUM_DCHANS; which++) {
				if (!pri->dchans[which])
					break;
				if (fds[which].revents & POLLPRI) {
					/* Check for an event */
					x = 0;
					res = ioctl(pri->fds[which], DAHDI_GETEVENT, &x);
					if (x) {
						ast_log(LOG_NOTICE, "PRI got event: %s (%d) on %s D-channel of span %d\n", event2str(x), x, pri_order(which), pri->span);
						manager_event(EVENT_FLAG_SYSTEM, "PRIEvent",
							"PRIEvent: %s\r\n"
							"PRIEventCode: %d\r\n"
							"D-channel: %s\r\n"
							"Span: %d\r\n",
							event2str(x),
							x,
							pri_order(which),
							pri->span
							);
					}
					/* Keep track of alarm state */
					if (x == DAHDI_EVENT_ALARM) {
						pri->dchanavail[which] &= ~(DCHAN_NOTINALARM | DCHAN_UP);
						pri_find_dchan(pri);
					} else if (x == DAHDI_EVENT_NOALARM) {
						pri->dchanavail[which] |= DCHAN_NOTINALARM;
						pri_restart(pri->dchans[which]);
					}

					ast_debug(1, "Got event %s (%d) on D-channel for span %d\n", event2str(x), x, pri->span);
				} else if (fds[which].revents & POLLIN) {
					e = pri_check_event(pri->dchans[which]);
				}
				if (e)
					break;
			}
		} else if (errno != EINTR)
			ast_log(LOG_WARNING, "pri_event returned error %d (%s)\n", errno, strerror(errno));

		if (e) {
			if (pri->debug)
				pri_dump_event(pri->dchans[which], e);

			if (e->e != PRI_EVENT_DCHAN_DOWN) {
				if (!(pri->dchanavail[which] & DCHAN_UP)) {
					ast_verb(2, "%s D-Channel on span %d up\n", pri_order(which), pri->span);
				}
				pri->dchanavail[which] |= DCHAN_UP;
			} else {
				if (pri->dchanavail[which] & DCHAN_UP) {
					ast_verb(2, "%s D-Channel on span %d down\n", pri_order(which), pri->span);
				}
				pri->dchanavail[which] &= ~DCHAN_UP;
			}

			if ((e->e != PRI_EVENT_DCHAN_UP) && (e->e != PRI_EVENT_DCHAN_DOWN) && (pri->pri != pri->dchans[which]))
				/* Must be an NFAS group that has the secondary dchan active */
				pri->pri = pri->dchans[which];

			switch (e->e) {
			case PRI_EVENT_DCHAN_UP:
				if (!pri->pri) pri_find_dchan(pri);

				/* Note presense of D-channel */
				time(&pri->lastreset);

				/* Restart in 5 seconds */
				if (pri->resetinterval > -1) {
					pri->lastreset -= pri->resetinterval;
					pri->lastreset += 5;
				}
				pri->resetting = 0;
				/* Take the channels from inalarm condition */
				for (i = 0; i < pri->numchans; i++)
					if (pri->pvts[i]) {
						pri->pvts[i]->inalarm = 0;
					}
				break;
			case PRI_EVENT_DCHAN_DOWN:
				pri_find_dchan(pri);
				if (!pri_is_up(pri)) {
					pri->resetting = 0;
					/* Hangup active channels and put them in alarm mode */
					for (i = 0; i < pri->numchans; i++) {
						struct dahdi_pvt *p = pri->pvts[i];
						if (p) {
							if (!p->pri || !p->pri->pri || pri_get_timer(p->pri->pri, PRI_TIMER_T309) < 0) {
								/* T309 is not enabled : hangup calls when alarm occurs */
								if (p->call) {
									if (p->pri && p->pri->pri) {
										pri_hangup(p->pri->pri, p->call, -1);
										pri_destroycall(p->pri->pri, p->call);
										p->call = NULL;
									} else
										ast_log(LOG_WARNING, "The PRI Call have not been destroyed\n");
								}
								if (p->realcall) {
									pri_hangup_all(p->realcall, pri);
								} else if (p->owner)
									p->owner->_softhangup |= AST_SOFTHANGUP_DEV;
							}
							p->inalarm = 1;
						}
					}
				}
				break;
			case PRI_EVENT_RESTART:
				if (e->restart.channel > -1) {
					chanpos = pri_find_principle(pri, e->restart.channel);
					if (chanpos < 0)
						ast_log(LOG_WARNING, "Restart requested on odd/unavailable channel number %d/%d on span %d\n",
							PRI_SPAN(e->restart.channel), PRI_CHANNEL(e->restart.channel), pri->span);
					else {
						ast_verb(3, "B-channel %d/%d restarted on span %d\n",
								PRI_SPAN(e->restart.channel), PRI_CHANNEL(e->restart.channel), pri->span);
						ast_mutex_lock(&pri->pvts[chanpos]->lock);
						if (pri->pvts[chanpos]->call) {
							pri_destroycall(pri->pri, pri->pvts[chanpos]->call);
							pri->pvts[chanpos]->call = NULL;
						}
						/* Force soft hangup if appropriate */
						if (pri->pvts[chanpos]->realcall)
							pri_hangup_all(pri->pvts[chanpos]->realcall, pri);
						else if (pri->pvts[chanpos]->owner)
							pri->pvts[chanpos]->owner->_softhangup |= AST_SOFTHANGUP_DEV;
						ast_mutex_unlock(&pri->pvts[chanpos]->lock);
					}
				} else {
					ast_verb(3, "Restart on requested on entire span %d\n", pri->span);
					for (x = 0; x < pri->numchans; x++)
						if (pri->pvts[x]) {
							ast_mutex_lock(&pri->pvts[x]->lock);
							if (pri->pvts[x]->call) {
								pri_destroycall(pri->pri, pri->pvts[x]->call);
								pri->pvts[x]->call = NULL;
							}
							if (pri->pvts[chanpos]->realcall)
								pri_hangup_all(pri->pvts[chanpos]->realcall, pri);
 							else if (pri->pvts[x]->owner)
								pri->pvts[x]->owner->_softhangup |= AST_SOFTHANGUP_DEV;
							ast_mutex_unlock(&pri->pvts[x]->lock);
						}
				}
				break;
			case PRI_EVENT_KEYPAD_DIGIT:
				chanpos = pri_find_principle(pri, e->digit.channel);
				if (chanpos < 0) {
					ast_log(LOG_WARNING, "KEYPAD_DIGITs received on unconfigured channel %d/%d span %d\n",
						PRI_SPAN(e->digit.channel), PRI_CHANNEL(e->digit.channel), pri->span);
				} else {
					chanpos = pri_fixup_principle(pri, chanpos, e->digit.call);
					if (chanpos > -1) {
						ast_mutex_lock(&pri->pvts[chanpos]->lock);
						/* queue DTMF frame if the PBX for this call was already started (we're forwarding KEYPAD_DIGITs further on */
						if ((pri->overlapdial & DAHDI_OVERLAPDIAL_INCOMING) && pri->pvts[chanpos]->call==e->digit.call && pri->pvts[chanpos]->owner) {
							/* how to do that */
							int digitlen = strlen(e->digit.digits);
							char digit;
							int i;
							for (i = 0; i < digitlen; i++) {
								digit = e->digit.digits[i];
								{
									struct ast_frame f = { AST_FRAME_DTMF, digit, };
									dahdi_queue_frame(pri->pvts[chanpos], &f, pri);
								}
							}
						}
						ast_mutex_unlock(&pri->pvts[chanpos]->lock);
					}
				}
				break;

			case PRI_EVENT_INFO_RECEIVED:
				chanpos = pri_find_principle(pri, e->ring.channel);
				if (chanpos < 0) {
					ast_log(LOG_WARNING, "INFO received on unconfigured channel %d/%d span %d\n",
						PRI_SPAN(e->ring.channel), PRI_CHANNEL(e->ring.channel), pri->span);
				} else {
					chanpos = pri_fixup_principle(pri, chanpos, e->ring.call);
					if (chanpos > -1) {
						ast_mutex_lock(&pri->pvts[chanpos]->lock);
						/* queue DTMF frame if the PBX for this call was already started (we're forwarding INFORMATION further on */
						if ((pri->overlapdial & DAHDI_OVERLAPDIAL_INCOMING) && pri->pvts[chanpos]->call==e->ring.call && pri->pvts[chanpos]->owner) {
							/* how to do that */
							int digitlen = strlen(e->ring.callednum);
							char digit;
							int i;
							for (i = 0; i < digitlen; i++) {
								digit = e->ring.callednum[i];
								{
									struct ast_frame f = { AST_FRAME_DTMF, digit, };
									dahdi_queue_frame(pri->pvts[chanpos], &f, pri);
								}
							}
						}
						ast_mutex_unlock(&pri->pvts[chanpos]->lock);
					}
				}
				break;
			case PRI_EVENT_RING:
				crv = NULL;
				if (e->ring.channel == -1)
					chanpos = pri_find_empty_chan(pri, 1);
				else
					chanpos = pri_find_principle(pri, e->ring.channel);
				/* if no channel specified find one empty */
				if (chanpos < 0) {
					ast_log(LOG_WARNING, "Ring requested on unconfigured channel %d/%d span %d\n",
						PRI_SPAN(e->ring.channel), PRI_CHANNEL(e->ring.channel), pri->span);
				} else {
					ast_mutex_lock(&pri->pvts[chanpos]->lock);
					if (pri->pvts[chanpos]->owner) {
						if (pri->pvts[chanpos]->call == e->ring.call) {
							ast_log(LOG_WARNING, "Duplicate setup requested on channel %d/%d already in use on span %d\n",
								PRI_SPAN(e->ring.channel), PRI_CHANNEL(e->ring.channel), pri->span);
							break;
						} else {
							/* This is where we handle initial glare */
							ast_debug(1, "Ring requested on channel %d/%d already in use or previously requested on span %d.  Attempting to renegotiate channel.\n",
							PRI_SPAN(e->ring.channel), PRI_CHANNEL(e->ring.channel), pri->span);
							ast_mutex_unlock(&pri->pvts[chanpos]->lock);
							chanpos = -1;
						}
					}
					if (chanpos > -1)
						ast_mutex_unlock(&pri->pvts[chanpos]->lock);
				}
				if ((chanpos < 0) && (e->ring.flexible))
					chanpos = pri_find_empty_chan(pri, 1);
				if (chanpos > -1) {
					ast_mutex_lock(&pri->pvts[chanpos]->lock);
					if (pri->switchtype == PRI_SWITCH_GR303_TMC) {
						/* Should be safe to lock CRV AFAIK while bearer is still locked */
						crv = pri_find_crv(pri, pri_get_crv(pri->pri, e->ring.call, NULL));
						if (crv)
							ast_mutex_lock(&crv->lock);
						if (!crv || crv->owner) {
							pri->pvts[chanpos]->call = NULL;
							if (crv) {
								if (crv->owner)
									crv->owner->_softhangup |= AST_SOFTHANGUP_DEV;
								ast_log(LOG_WARNING, "Call received for busy CRV %d on span %d\n", pri_get_crv(pri->pri, e->ring.call, NULL), pri->span);
							} else
								ast_log(LOG_NOTICE, "Call received for unconfigured CRV %d on span %d\n", pri_get_crv(pri->pri, e->ring.call, NULL), pri->span);
							pri_hangup(pri->pri, e->ring.call, PRI_CAUSE_INVALID_CALL_REFERENCE);
							if (crv)
								ast_mutex_unlock(&crv->lock);
							ast_mutex_unlock(&pri->pvts[chanpos]->lock);
							break;
						}
					}
					pri->pvts[chanpos]->call = e->ring.call;
					apply_plan_to_number(plancallingnum, sizeof(plancallingnum), pri, e->ring.callingnum, e->ring.callingplan);
					if (pri->pvts[chanpos]->use_callerid) {
						ast_shrink_phone_number(plancallingnum);
						ast_copy_string(pri->pvts[chanpos]->cid_num, plancallingnum, sizeof(pri->pvts[chanpos]->cid_num));
#ifdef PRI_ANI
						if (!ast_strlen_zero(e->ring.callingani)) {
							apply_plan_to_number(plancallingani, sizeof(plancallingani), pri, e->ring.callingani, e->ring.callingplanani);
							ast_shrink_phone_number(plancallingani);
							ast_copy_string(pri->pvts[chanpos]->cid_ani, plancallingani, sizeof(pri->pvts[chanpos]->cid_ani));
						} else {
							pri->pvts[chanpos]->cid_ani[0] = '\0';
						}
#endif
						ast_copy_string(pri->pvts[chanpos]->cid_name, e->ring.callingname, sizeof(pri->pvts[chanpos]->cid_name));
						pri->pvts[chanpos]->cid_ton = e->ring.callingplan; /* this is the callingplan (TON/NPI), e->ring.callingplan>>4 would be the TON */
					} else {
						pri->pvts[chanpos]->cid_num[0] = '\0';
						pri->pvts[chanpos]->cid_ani[0] = '\0';
						pri->pvts[chanpos]->cid_name[0] = '\0';
						pri->pvts[chanpos]->cid_ton = 0;
					}
					apply_plan_to_number(pri->pvts[chanpos]->rdnis, sizeof(pri->pvts[chanpos]->rdnis), pri,
							     e->ring.redirectingnum, e->ring.callingplanrdnis);
					/* If immediate=yes go to s|1 */
					if (pri->pvts[chanpos]->immediate) {
						ast_verb(3, "Going to extension s|1 because of immediate=yes\n");
						pri->pvts[chanpos]->exten[0] = 's';
						pri->pvts[chanpos]->exten[1] = '\0';
					}
					/* Get called number */
					else if (!ast_strlen_zero(e->ring.callednum)) {
						ast_copy_string(pri->pvts[chanpos]->exten, e->ring.callednum, sizeof(pri->pvts[chanpos]->exten));
						ast_copy_string(pri->pvts[chanpos]->dnid, e->ring.callednum, sizeof(pri->pvts[chanpos]->dnid));
					} else if (pri->overlapdial)
						pri->pvts[chanpos]->exten[0] = '\0';
					else {
						/* Some PRI circuits are set up to send _no_ digits.  Handle them as 's'. */
						pri->pvts[chanpos]->exten[0] = 's';
						pri->pvts[chanpos]->exten[1] = '\0';
					}
					/* Set DNID on all incoming calls -- even immediate */
					if (!ast_strlen_zero(e->ring.callednum))
						ast_copy_string(pri->pvts[chanpos]->dnid, e->ring.callednum, sizeof(pri->pvts[chanpos]->dnid));
					/* No number yet, but received "sending complete"? */
					if (e->ring.complete && (ast_strlen_zero(e->ring.callednum))) {
						ast_verb(3, "Going to extension s|1 because of Complete received\n");
						pri->pvts[chanpos]->exten[0] = 's';
						pri->pvts[chanpos]->exten[1] = '\0';
					}
					/* Make sure extension exists (or in overlap dial mode, can exist) */
					if (((pri->overlapdial & DAHDI_OVERLAPDIAL_INCOMING) && ast_canmatch_extension(NULL, pri->pvts[chanpos]->context, pri->pvts[chanpos]->exten, 1, pri->pvts[chanpos]->cid_num)) ||
						ast_exists_extension(NULL, pri->pvts[chanpos]->context, pri->pvts[chanpos]->exten, 1, pri->pvts[chanpos]->cid_num)) {
						/* Setup law */
						int law;
						if (pri->switchtype != PRI_SWITCH_GR303_TMC) {
							/* Set to audio mode at this point */
							law = 1;
							if (ioctl(pri->pvts[chanpos]->subs[SUB_REAL].dfd, DAHDI_AUDIOMODE, &law) == -1)
								ast_log(LOG_WARNING, "Unable to set audio mode on channel %d to %d: %s\n", pri->pvts[chanpos]->channel, law, strerror(errno));
						}
						if (e->ring.layer1 == PRI_LAYER_1_ALAW)
							law = DAHDI_LAW_ALAW;
						else
							law = DAHDI_LAW_MULAW;
						res = dahdi_setlaw(pri->pvts[chanpos]->subs[SUB_REAL].dfd, law);
						if (res < 0)
							ast_log(LOG_WARNING, "Unable to set law on channel %d\n", pri->pvts[chanpos]->channel);
						res = set_actual_gain(pri->pvts[chanpos]->subs[SUB_REAL].dfd, 0, pri->pvts[chanpos]->rxgain, pri->pvts[chanpos]->txgain, law);
						if (res < 0)
							ast_log(LOG_WARNING, "Unable to set gains on channel %d\n", pri->pvts[chanpos]->channel);
						if (e->ring.complete || !(pri->overlapdial & DAHDI_OVERLAPDIAL_INCOMING)) {
							/* Just announce proceeding */
							pri->pvts[chanpos]->proceeding = 1;
							pri_proceeding(pri->pri, e->ring.call, PVT_TO_CHANNEL(pri->pvts[chanpos]), 0);
						} else {
							if (pri->switchtype != PRI_SWITCH_GR303_TMC)
								pri_need_more_info(pri->pri, e->ring.call, PVT_TO_CHANNEL(pri->pvts[chanpos]), 1);
							else
								pri_answer(pri->pri, e->ring.call, PVT_TO_CHANNEL(pri->pvts[chanpos]), 1);
						}
						/* Get the use_callingpres state */
						pri->pvts[chanpos]->callingpres = e->ring.callingpres;

						/* Start PBX */
						if (!e->ring.complete && (pri->overlapdial & DAHDI_OVERLAPDIAL_INCOMING) && ast_matchmore_extension(NULL, pri->pvts[chanpos]->context, pri->pvts[chanpos]->exten, 1, pri->pvts[chanpos]->cid_num)) {
							/* Release the PRI lock while we create the channel */
							ast_mutex_unlock(&pri->lock);
							if (crv) {
								/* Set bearer and such */
								pri_assign_bearer(crv, pri, pri->pvts[chanpos]);
								c = dahdi_new(crv, AST_STATE_RESERVED, 0, SUB_REAL, law, e->ring.ctype);
								pri->pvts[chanpos]->owner = &inuse;
								ast_debug(1, "Started up crv %d:%d on bearer channel %d\n", pri->trunkgroup, crv->channel, crv->bearer->channel);
							} else {
								c = dahdi_new(pri->pvts[chanpos], AST_STATE_RESERVED, 0, SUB_REAL, law, e->ring.ctype);
							}

							ast_mutex_unlock(&pri->pvts[chanpos]->lock);

							if (!ast_strlen_zero(e->ring.callingsubaddr)) {
								pbx_builtin_setvar_helper(c, "CALLINGSUBADDR", e->ring.callingsubaddr);
							}
							if (e->ring.ani2 >= 0) {
								snprintf(ani2str, 5, "%.2d", e->ring.ani2);
								pbx_builtin_setvar_helper(c, "ANI2", ani2str);
								pri->pvts[chanpos]->cid_ani2 = e->ring.ani2;
							}

#ifdef SUPPORT_USERUSER
							if (!ast_strlen_zero(e->ring.useruserinfo)) {
								pbx_builtin_setvar_helper(c, "USERUSERINFO", e->ring.useruserinfo);
							}
#endif

							snprintf(calledtonstr, sizeof(calledtonstr), "%d", e->ring.calledplan);
							pbx_builtin_setvar_helper(c, "CALLEDTON", calledtonstr);
							if (e->ring.redirectingreason >= 0)
								pbx_builtin_setvar_helper(c, "PRIREDIRECTREASON", redirectingreason2str(e->ring.redirectingreason));

							ast_mutex_lock(&pri->pvts[chanpos]->lock);
							ast_mutex_lock(&pri->lock);
							if (c && !ast_pthread_create_detached(&threadid, NULL, ss_thread, c)) {
								ast_verb(3, "Accepting overlap call from '%s' to '%s' on channel %d/%d, span %d\n",
										plancallingnum, S_OR(pri->pvts[chanpos]->exten, "<unspecified>"),
										pri->pvts[chanpos]->logicalspan, pri->pvts[chanpos]->prioffset, pri->span);
							} else {
								ast_log(LOG_WARNING, "Unable to start PBX on channel %d/%d, span %d\n",
									pri->pvts[chanpos]->logicalspan, pri->pvts[chanpos]->prioffset, pri->span);
								if (c)
									ast_hangup(c);
								else {
									pri_hangup(pri->pri, e->ring.call, PRI_CAUSE_SWITCH_CONGESTION);
									pri->pvts[chanpos]->call = NULL;
								}
							}
						} else  {
							ast_mutex_unlock(&pri->lock);
							/* Release PRI lock while we create the channel */
							c = dahdi_new(pri->pvts[chanpos], AST_STATE_RING, 1, SUB_REAL, law, e->ring.ctype);
							if (c) {
								char calledtonstr[10];

								ast_mutex_unlock(&pri->pvts[chanpos]->lock);

								if (e->ring.ani2 >= 0) {
									snprintf(ani2str, 5, "%d", e->ring.ani2);
									pbx_builtin_setvar_helper(c, "ANI2", ani2str);
									pri->pvts[chanpos]->cid_ani2 = e->ring.ani2;
								}

#ifdef SUPPORT_USERUSER
								if (!ast_strlen_zero(e->ring.useruserinfo)) {
									pbx_builtin_setvar_helper(c, "USERUSERINFO", e->ring.useruserinfo);
								}
#endif

								if (e->ring.redirectingreason >= 0)
									pbx_builtin_setvar_helper(c, "PRIREDIRECTREASON", redirectingreason2str(e->ring.redirectingreason));

								snprintf(calledtonstr, sizeof(calledtonstr), "%d", e->ring.calledplan);
								pbx_builtin_setvar_helper(c, "CALLEDTON", calledtonstr);

								ast_mutex_lock(&pri->pvts[chanpos]->lock);
								ast_mutex_lock(&pri->lock);

								ast_verb(3, "Accepting call from '%s' to '%s' on channel %d/%d, span %d\n",
										plancallingnum, pri->pvts[chanpos]->exten,
										pri->pvts[chanpos]->logicalspan, pri->pvts[chanpos]->prioffset, pri->span);

								dahdi_enable_ec(pri->pvts[chanpos]);
							} else {

								ast_mutex_lock(&pri->lock);

								ast_log(LOG_WARNING, "Unable to start PBX on channel %d/%d, span %d\n",
									pri->pvts[chanpos]->logicalspan, pri->pvts[chanpos]->prioffset, pri->span);
								pri_hangup(pri->pri, e->ring.call, PRI_CAUSE_SWITCH_CONGESTION);
								pri->pvts[chanpos]->call = NULL;
							}
						}
					} else {
						ast_verb(3, "Extension '%s' in context '%s' from '%s' does not exist.  Rejecting call on channel %d/%d, span %d\n",
								pri->pvts[chanpos]->exten, pri->pvts[chanpos]->context, pri->pvts[chanpos]->cid_num, pri->pvts[chanpos]->logicalspan,
									pri->pvts[chanpos]->prioffset, pri->span);
						pri_hangup(pri->pri, e->ring.call, PRI_CAUSE_UNALLOCATED);
						pri->pvts[chanpos]->call = NULL;
						pri->pvts[chanpos]->exten[0] = '\0';
					}
					if (crv)
						ast_mutex_unlock(&crv->lock);
					ast_mutex_unlock(&pri->pvts[chanpos]->lock);
				} else {
					if (e->ring.flexible)
						pri_hangup(pri->pri, e->ring.call, PRI_CAUSE_NORMAL_CIRCUIT_CONGESTION);
					else
						pri_hangup(pri->pri, e->ring.call, PRI_CAUSE_REQUESTED_CHAN_UNAVAIL);
				}
				break;
			case PRI_EVENT_RINGING:
				chanpos = pri_find_principle(pri, e->ringing.channel);
				if (chanpos < 0) {
					ast_log(LOG_WARNING, "Ringing requested on unconfigured channel %d/%d span %d\n",
						PRI_SPAN(e->ringing.channel), PRI_CHANNEL(e->ringing.channel), pri->span);
				} else {
					chanpos = pri_fixup_principle(pri, chanpos, e->ringing.call);
					if (chanpos < 0) {
						ast_log(LOG_WARNING, "Ringing requested on channel %d/%d not in use on span %d\n",
							PRI_SPAN(e->ringing.channel), PRI_CHANNEL(e->ringing.channel), pri->span);
					} else {
						ast_mutex_lock(&pri->pvts[chanpos]->lock);
						if (ast_strlen_zero(pri->pvts[chanpos]->dop.dialstr)) {
							dahdi_enable_ec(pri->pvts[chanpos]);
							pri->pvts[chanpos]->subs[SUB_REAL].needringing = 1;
							pri->pvts[chanpos]->alerting = 1;
						} else
							ast_debug(1, "Deferring ringing notification because of extra digits to dial...\n");

#ifdef PRI_PROGRESS_MASK
						if (e->ringing.progressmask & PRI_PROG_INBAND_AVAILABLE) {
#else
						if (e->ringing.progress == 8) {
#endif
							/* Now we can do call progress detection */
							if (pri->pvts[chanpos]->dsp && pri->pvts[chanpos]->dsp_features) {
								/* RINGING detection isn't required because we got ALERTING signal */
								ast_dsp_set_features(pri->pvts[chanpos]->dsp, pri->pvts[chanpos]->dsp_features & ~DSP_PROGRESS_RINGING);
								pri->pvts[chanpos]->dsp_features = 0;
							}
						}

#ifdef SUPPORT_USERUSER
						if (!ast_strlen_zero(e->ringing.useruserinfo)) {
							struct ast_channel *owner = pri->pvts[chanpos]->owner;
							ast_mutex_unlock(&pri->pvts[chanpos]->lock);
							pbx_builtin_setvar_helper(owner, "USERUSERINFO", e->ringing.useruserinfo);
							ast_mutex_lock(&pri->pvts[chanpos]->lock);
						}
#endif

						ast_mutex_unlock(&pri->pvts[chanpos]->lock);
					}
				}
				break;
			case PRI_EVENT_PROGRESS:
				/* Get chan value if e->e is not PRI_EVNT_RINGING */
				chanpos = pri_find_principle(pri, e->proceeding.channel);
				if (chanpos > -1) {
#ifdef PRI_PROGRESS_MASK
					if ((!pri->pvts[chanpos]->progress) || (e->proceeding.progressmask & PRI_PROG_INBAND_AVAILABLE)) {
#else
					if ((!pri->pvts[chanpos]->progress) || (e->proceeding.progress == 8)) {
#endif
						struct ast_frame f = { AST_FRAME_CONTROL, AST_CONTROL_PROGRESS, };

						if (e->proceeding.cause > -1) {
							ast_verb(3, "PROGRESS with cause code %d received\n", e->proceeding.cause);

							/* Work around broken, out of spec USER_BUSY cause in a progress message */
							if (e->proceeding.cause == AST_CAUSE_USER_BUSY) {
								if (pri->pvts[chanpos]->owner) {
									ast_verb(3, "PROGRESS with 'user busy' received, signalling AST_CONTROL_BUSY instead of AST_CONTROL_PROGRESS\n");

									pri->pvts[chanpos]->owner->hangupcause = e->proceeding.cause;
									f.subclass = AST_CONTROL_BUSY;
								}
							}
						}

						ast_mutex_lock(&pri->pvts[chanpos]->lock);
						ast_debug(1, "Queuing frame from PRI_EVENT_PROGRESS on channel %d/%d span %d\n",
							pri->pvts[chanpos]->logicalspan, pri->pvts[chanpos]->prioffset,pri->span);
						dahdi_queue_frame(pri->pvts[chanpos], &f, pri);
#ifdef PRI_PROGRESS_MASK
						if (e->proceeding.progressmask & PRI_PROG_INBAND_AVAILABLE) {
#else
						if (e->proceeding.progress == 8) {
#endif
							/* Now we can do call progress detection */
							if (pri->pvts[chanpos]->dsp && pri->pvts[chanpos]->dsp_features) {
								ast_dsp_set_features(pri->pvts[chanpos]->dsp, pri->pvts[chanpos]->dsp_features);
								pri->pvts[chanpos]->dsp_features = 0;
							}
						}
						pri->pvts[chanpos]->progress = 1;
						pri->pvts[chanpos]->dialing = 0;
						ast_mutex_unlock(&pri->pvts[chanpos]->lock);
					}
				}
				break;
			case PRI_EVENT_PROCEEDING:
				chanpos = pri_find_principle(pri, e->proceeding.channel);
				if (chanpos > -1) {
					if (!pri->pvts[chanpos]->proceeding) {
						struct ast_frame f = { AST_FRAME_CONTROL, AST_CONTROL_PROCEEDING, };

						ast_mutex_lock(&pri->pvts[chanpos]->lock);
						ast_debug(1, "Queuing frame from PRI_EVENT_PROCEEDING on channel %d/%d span %d\n",
							pri->pvts[chanpos]->logicalspan, pri->pvts[chanpos]->prioffset,pri->span);
						dahdi_queue_frame(pri->pvts[chanpos], &f, pri);
#ifdef PRI_PROGRESS_MASK
						if (e->proceeding.progressmask & PRI_PROG_INBAND_AVAILABLE) {
#else
						if (e->proceeding.progress == 8) {
#endif
							/* Now we can do call progress detection */
							if (pri->pvts[chanpos]->dsp && pri->pvts[chanpos]->dsp_features) {
								ast_dsp_set_features(pri->pvts[chanpos]->dsp, pri->pvts[chanpos]->dsp_features);
								pri->pvts[chanpos]->dsp_features = 0;
							}
							/* Bring voice path up */
							f.subclass = AST_CONTROL_PROGRESS;
							dahdi_queue_frame(pri->pvts[chanpos], &f, pri);
						}
						pri->pvts[chanpos]->proceeding = 1;
						pri->pvts[chanpos]->dialing = 0;
						ast_mutex_unlock(&pri->pvts[chanpos]->lock);
					}
				}
				break;
			case PRI_EVENT_FACNAME:
				chanpos = pri_find_principle(pri, e->facname.channel);
				if (chanpos < 0) {
					ast_log(LOG_WARNING, "Facility Name requested on unconfigured channel %d/%d span %d\n",
						PRI_SPAN(e->facname.channel), PRI_CHANNEL(e->facname.channel), pri->span);
				} else {
					chanpos = pri_fixup_principle(pri, chanpos, e->facname.call);
					if (chanpos < 0) {
						ast_log(LOG_WARNING, "Facility Name requested on channel %d/%d not in use on span %d\n",
							PRI_SPAN(e->facname.channel), PRI_CHANNEL(e->facname.channel), pri->span);
					} else {
						/* Re-use *69 field for PRI */
						ast_mutex_lock(&pri->pvts[chanpos]->lock);
						ast_copy_string(pri->pvts[chanpos]->lastcid_num, e->facname.callingnum, sizeof(pri->pvts[chanpos]->lastcid_num));
						ast_copy_string(pri->pvts[chanpos]->lastcid_name, e->facname.callingname, sizeof(pri->pvts[chanpos]->lastcid_name));
						pri->pvts[chanpos]->subs[SUB_REAL].needcallerid =1;
						dahdi_enable_ec(pri->pvts[chanpos]);
						ast_mutex_unlock(&pri->pvts[chanpos]->lock);
					}
				}
				break;
			case PRI_EVENT_ANSWER:
				chanpos = pri_find_principle(pri, e->answer.channel);
				if (chanpos < 0) {
					ast_log(LOG_WARNING, "Answer on unconfigured channel %d/%d span %d\n",
						PRI_SPAN(e->answer.channel), PRI_CHANNEL(e->answer.channel), pri->span);
				} else {
					chanpos = pri_fixup_principle(pri, chanpos, e->answer.call);
					if (chanpos < 0) {
						ast_log(LOG_WARNING, "Answer requested on channel %d/%d not in use on span %d\n",
							PRI_SPAN(e->answer.channel), PRI_CHANNEL(e->answer.channel), pri->span);
					} else {
						ast_mutex_lock(&pri->pvts[chanpos]->lock);
						/* Now we can do call progress detection */

						/* We changed this so it turns on the DSP no matter what... progress or no progress.
						 * By this time, we need DTMF detection and other features that were previously disabled
						 * -- Matt F */
						if (pri->pvts[chanpos]->dsp && pri->pvts[chanpos]->dsp_features) {
							ast_dsp_set_features(pri->pvts[chanpos]->dsp, pri->pvts[chanpos]->dsp_features);
							pri->pvts[chanpos]->dsp_features = 0;
						}
						if (pri->pvts[chanpos]->realcall && (pri->pvts[chanpos]->realcall->sig == SIG_FXSKS)) {
							ast_debug(1, "Starting up GR-303 trunk now that we got CONNECT...\n");
							x = DAHDI_START;
							res = ioctl(pri->pvts[chanpos]->subs[SUB_REAL].dfd, DAHDI_HOOK, &x);
							if (res < 0) {
								if (errno != EINPROGRESS) {
									ast_log(LOG_WARNING, "Unable to start channel: %s\n", strerror(errno));
								}
							}
						} else if (!ast_strlen_zero(pri->pvts[chanpos]->dop.dialstr)) {
							pri->pvts[chanpos]->dialing = 1;
							/* Send any "w" waited stuff */
							res = ioctl(pri->pvts[chanpos]->subs[SUB_REAL].dfd, DAHDI_DIAL, &pri->pvts[chanpos]->dop);
							if (res < 0) {
								ast_log(LOG_WARNING, "Unable to initiate dialing on trunk channel %d: %s\n", pri->pvts[chanpos]->channel, strerror(errno));
								pri->pvts[chanpos]->dop.dialstr[0] = '\0';
							} else
								ast_debug(1, "Sent deferred digit string: %s\n", pri->pvts[chanpos]->dop.dialstr);

							pri->pvts[chanpos]->dop.dialstr[0] = '\0';
						} else if (pri->pvts[chanpos]->confirmanswer) {
							ast_debug(1, "Waiting on answer confirmation on channel %d!\n", pri->pvts[chanpos]->channel);
						} else {
							pri->pvts[chanpos]->subs[SUB_REAL].needanswer =1;
							/* Enable echo cancellation if it's not on already */
							dahdi_enable_ec(pri->pvts[chanpos]);
						}

#ifdef SUPPORT_USERUSER
						if (!ast_strlen_zero(e->answer.useruserinfo)) {
							struct ast_channel *owner = pri->pvts[chanpos]->owner;
							ast_mutex_unlock(&pri->pvts[chanpos]->lock);
							pbx_builtin_setvar_helper(owner, "USERUSERINFO", e->answer.useruserinfo);
							ast_mutex_lock(&pri->pvts[chanpos]->lock);
						}
#endif

						ast_mutex_unlock(&pri->pvts[chanpos]->lock);
					}
				}
				break;
			case PRI_EVENT_HANGUP:
				chanpos = pri_find_principle(pri, e->hangup.channel);
				if (chanpos < 0) {
					ast_log(LOG_WARNING, "Hangup requested on unconfigured channel %d/%d span %d\n",
						PRI_SPAN(e->hangup.channel), PRI_CHANNEL(e->hangup.channel), pri->span);
				} else {
					chanpos = pri_fixup_principle(pri, chanpos, e->hangup.call);
					if (chanpos > -1) {
						ast_mutex_lock(&pri->pvts[chanpos]->lock);
						if (!pri->pvts[chanpos]->alreadyhungup) {
							/* we're calling here dahdi_hangup so once we get there we need to clear p->call after calling pri_hangup */
							pri->pvts[chanpos]->alreadyhungup = 1;
							if (pri->pvts[chanpos]->realcall)
								pri_hangup_all(pri->pvts[chanpos]->realcall, pri);
							else if (pri->pvts[chanpos]->owner) {
								/* Queue a BUSY instead of a hangup if our cause is appropriate */
								pri->pvts[chanpos]->owner->hangupcause = e->hangup.cause;
								if (pri->pvts[chanpos]->owner->_state == AST_STATE_UP)
									pri->pvts[chanpos]->owner->_softhangup |= AST_SOFTHANGUP_DEV;
								else {
									switch (e->hangup.cause) {
									case PRI_CAUSE_USER_BUSY:
										pri->pvts[chanpos]->subs[SUB_REAL].needbusy =1;
										break;
									case PRI_CAUSE_CALL_REJECTED:
									case PRI_CAUSE_NETWORK_OUT_OF_ORDER:
									case PRI_CAUSE_NORMAL_CIRCUIT_CONGESTION:
									case PRI_CAUSE_SWITCH_CONGESTION:
									case PRI_CAUSE_DESTINATION_OUT_OF_ORDER:
									case PRI_CAUSE_NORMAL_TEMPORARY_FAILURE:
										pri->pvts[chanpos]->subs[SUB_REAL].needcongestion =1;
										break;
									default:
										pri->pvts[chanpos]->owner->_softhangup |= AST_SOFTHANGUP_DEV;
									}
								}
							}
							ast_verb(3, "Channel %d/%d, span %d got hangup, cause %d\n",
									pri->pvts[chanpos]->logicalspan, pri->pvts[chanpos]->prioffset, pri->span, e->hangup.cause);
						} else {
							pri_hangup(pri->pri, pri->pvts[chanpos]->call, e->hangup.cause);
							pri->pvts[chanpos]->call = NULL;
						}
						if (e->hangup.cause == PRI_CAUSE_REQUESTED_CHAN_UNAVAIL) {
							ast_verb(3, "Forcing restart of channel %d/%d on span %d since channel reported in use\n",
									PRI_SPAN(e->hangup.channel), PRI_CHANNEL(e->hangup.channel), pri->span);
							pri_reset(pri->pri, PVT_TO_CHANNEL(pri->pvts[chanpos]));
							pri->pvts[chanpos]->resetting = 1;
						}
						if (e->hangup.aoc_units > -1)
							ast_verb(3, "Channel %d/%d, span %d received AOC-E charging %d unit%s\n",
									pri->pvts[chanpos]->logicalspan, pri->pvts[chanpos]->prioffset, pri->span, (int)e->hangup.aoc_units, (e->hangup.aoc_units == 1) ? "" : "s");

#ifdef SUPPORT_USERUSER
						if (pri->pvts[chanpos]->owner && !ast_strlen_zero(e->hangup.useruserinfo)) {
							struct ast_channel *owner = pri->pvts[chanpos]->owner;
							ast_mutex_unlock(&pri->pvts[chanpos]->lock);
							pbx_builtin_setvar_helper(owner, "USERUSERINFO", e->hangup.useruserinfo);
							ast_mutex_lock(&pri->pvts[chanpos]->lock);
						}
#endif

						ast_mutex_unlock(&pri->pvts[chanpos]->lock);
					} else {
						ast_log(LOG_WARNING, "Hangup on bad channel %d/%d on span %d\n",
							PRI_SPAN(e->hangup.channel), PRI_CHANNEL(e->hangup.channel), pri->span);
					}
				}
				break;
#ifndef PRI_EVENT_HANGUP_REQ
#error please update libpri
#endif
			case PRI_EVENT_HANGUP_REQ:
				chanpos = pri_find_principle(pri, e->hangup.channel);
				if (chanpos < 0) {
					ast_log(LOG_WARNING, "Hangup REQ requested on unconfigured channel %d/%d span %d\n",
						PRI_SPAN(e->hangup.channel), PRI_CHANNEL(e->hangup.channel), pri->span);
				} else {
					chanpos = pri_fixup_principle(pri, chanpos, e->hangup.call);
					if (chanpos > -1) {
						ast_mutex_lock(&pri->pvts[chanpos]->lock);
						if (pri->pvts[chanpos]->realcall)
							pri_hangup_all(pri->pvts[chanpos]->realcall, pri);
						else if (pri->pvts[chanpos]->owner) {
							pri->pvts[chanpos]->owner->hangupcause = e->hangup.cause;
							if (pri->pvts[chanpos]->owner->_state == AST_STATE_UP)
								pri->pvts[chanpos]->owner->_softhangup |= AST_SOFTHANGUP_DEV;
							else {
								switch (e->hangup.cause) {
									case PRI_CAUSE_USER_BUSY:
										pri->pvts[chanpos]->subs[SUB_REAL].needbusy =1;
										break;
									case PRI_CAUSE_CALL_REJECTED:
									case PRI_CAUSE_NETWORK_OUT_OF_ORDER:
									case PRI_CAUSE_NORMAL_CIRCUIT_CONGESTION:
									case PRI_CAUSE_SWITCH_CONGESTION:
									case PRI_CAUSE_DESTINATION_OUT_OF_ORDER:
									case PRI_CAUSE_NORMAL_TEMPORARY_FAILURE:
										pri->pvts[chanpos]->subs[SUB_REAL].needcongestion =1;
										break;
									default:
										pri->pvts[chanpos]->owner->_softhangup |= AST_SOFTHANGUP_DEV;
								}
							}
							ast_verb(3, "Channel %d/%d, span %d got hangup request, cause %d\n", PRI_SPAN(e->hangup.channel), PRI_CHANNEL(e->hangup.channel), pri->span, e->hangup.cause);
							if (e->hangup.aoc_units > -1)
								ast_verb(3, "Channel %d/%d, span %d received AOC-E charging %d unit%s\n",
										pri->pvts[chanpos]->logicalspan, pri->pvts[chanpos]->prioffset, pri->span, (int)e->hangup.aoc_units, (e->hangup.aoc_units == 1) ? "" : "s");
						} else {
							pri_hangup(pri->pri, pri->pvts[chanpos]->call, e->hangup.cause);
							pri->pvts[chanpos]->call = NULL;
						}
						if (e->hangup.cause == PRI_CAUSE_REQUESTED_CHAN_UNAVAIL) {
							ast_verb(3, "Forcing restart of channel %d/%d span %d since channel reported in use\n",
									PRI_SPAN(e->hangup.channel), PRI_CHANNEL(e->hangup.channel), pri->span);
							pri_reset(pri->pri, PVT_TO_CHANNEL(pri->pvts[chanpos]));
							pri->pvts[chanpos]->resetting = 1;
						}

#ifdef SUPPORT_USERUSER
						if (!ast_strlen_zero(e->hangup.useruserinfo)) {
							struct ast_channel *owner = pri->pvts[chanpos]->owner;
							ast_mutex_unlock(&pri->pvts[chanpos]->lock);
							pbx_builtin_setvar_helper(owner, "USERUSERINFO", e->hangup.useruserinfo);
							ast_mutex_lock(&pri->pvts[chanpos]->lock);
						}
#endif

						ast_mutex_unlock(&pri->pvts[chanpos]->lock);
					} else {
						ast_log(LOG_WARNING, "Hangup REQ on bad channel %d/%d on span %d\n", PRI_SPAN(e->hangup.channel), PRI_CHANNEL(e->hangup.channel), pri->span);
					}
				}
				break;
			case PRI_EVENT_HANGUP_ACK:
				chanpos = pri_find_principle(pri, e->hangup.channel);
				if (chanpos < 0) {
					ast_log(LOG_WARNING, "Hangup ACK requested on unconfigured channel number %d/%d span %d\n",
						PRI_SPAN(e->hangup.channel), PRI_CHANNEL(e->hangup.channel), pri->span);
				} else {
					chanpos = pri_fixup_principle(pri, chanpos, e->hangup.call);
					if (chanpos > -1) {
						ast_mutex_lock(&pri->pvts[chanpos]->lock);
						pri->pvts[chanpos]->call = NULL;
						pri->pvts[chanpos]->resetting = 0;
						if (pri->pvts[chanpos]->owner) {
							ast_verb(3, "Channel %d/%d, span %d got hangup ACK\n", PRI_SPAN(e->hangup.channel), PRI_CHANNEL(e->hangup.channel), pri->span);
						}

#ifdef SUPPORT_USERUSER
						if (!ast_strlen_zero(e->hangup.useruserinfo)) {
							struct ast_channel *owner = pri->pvts[chanpos]->owner;
							ast_mutex_unlock(&pri->pvts[chanpos]->lock);
							pbx_builtin_setvar_helper(owner, "USERUSERINFO", e->hangup.useruserinfo);
							ast_mutex_lock(&pri->pvts[chanpos]->lock);
						}
#endif

						ast_mutex_unlock(&pri->pvts[chanpos]->lock);
					}
				}
				break;
			case PRI_EVENT_CONFIG_ERR:
				ast_log(LOG_WARNING, "PRI Error on span %d: %s\n", pri->trunkgroup, e->err.err);
				break;
			case PRI_EVENT_RESTART_ACK:
				chanpos = pri_find_principle(pri, e->restartack.channel);
				if (chanpos < 0) {
					/* Sometime switches (e.g. I421 / British Telecom) don't give us the
					   channel number, so we have to figure it out...  This must be why
					   everybody resets exactly a channel at a time. */
					for (x = 0; x < pri->numchans; x++) {
						if (pri->pvts[x] && pri->pvts[x]->resetting) {
							chanpos = x;
							ast_mutex_lock(&pri->pvts[chanpos]->lock);
							ast_debug(1, "Assuming restart ack is really for channel %d/%d span %d\n", pri->pvts[chanpos]->logicalspan,
								pri->pvts[chanpos]->prioffset, pri->span);
							if (pri->pvts[chanpos]->realcall)
								pri_hangup_all(pri->pvts[chanpos]->realcall, pri);
							else if (pri->pvts[chanpos]->owner) {
								ast_log(LOG_WARNING, "Got restart ack on channel %d/%d with owner on span %d\n", pri->pvts[chanpos]->logicalspan,
									pri->pvts[chanpos]->prioffset, pri->span);
								pri->pvts[chanpos]->owner->_softhangup |= AST_SOFTHANGUP_DEV;
							}
							pri->pvts[chanpos]->resetting = 0;
							ast_verb(3, "B-channel %d/%d successfully restarted on span %d\n", pri->pvts[chanpos]->logicalspan,
									pri->pvts[chanpos]->prioffset, pri->span);
							ast_mutex_unlock(&pri->pvts[chanpos]->lock);
							if (pri->resetting)
								pri_check_restart(pri);
							break;
						}
					}
					if (chanpos < 0) {
						ast_log(LOG_WARNING, "Restart ACK requested on strange channel %d/%d span %d\n",
							PRI_SPAN(e->restartack.channel), PRI_CHANNEL(e->restartack.channel), pri->span);
					}
				} else {
					if (pri->pvts[chanpos]) {
						ast_mutex_lock(&pri->pvts[chanpos]->lock);
						if (pri->pvts[chanpos]->realcall)
							pri_hangup_all(pri->pvts[chanpos]->realcall, pri);
						else if (pri->pvts[chanpos]->owner) {
							ast_log(LOG_WARNING, "Got restart ack on channel %d/%d span %d with owner\n",
								PRI_SPAN(e->restartack.channel), PRI_CHANNEL(e->restartack.channel), pri->span);
							pri->pvts[chanpos]->owner->_softhangup |= AST_SOFTHANGUP_DEV;
						}
						pri->pvts[chanpos]->resetting = 0;
						pri->pvts[chanpos]->inservice = 1;
						ast_verb(3, "B-channel %d/%d successfully restarted on span %d\n", pri->pvts[chanpos]->logicalspan,
									pri->pvts[chanpos]->prioffset, pri->span);
						ast_mutex_unlock(&pri->pvts[chanpos]->lock);
						if (pri->resetting)
							pri_check_restart(pri);
					}
				}
				break;
			case PRI_EVENT_SETUP_ACK:
				chanpos = pri_find_principle(pri, e->setup_ack.channel);
				if (chanpos < 0) {
					ast_log(LOG_WARNING, "Received SETUP_ACKNOWLEDGE on unconfigured channel %d/%d span %d\n",
						PRI_SPAN(e->setup_ack.channel), PRI_CHANNEL(e->setup_ack.channel), pri->span);
				} else {
					chanpos = pri_fixup_principle(pri, chanpos, e->setup_ack.call);
					if (chanpos > -1) {
						ast_mutex_lock(&pri->pvts[chanpos]->lock);
						pri->pvts[chanpos]->setup_ack = 1;
						/* Send any queued digits */
						for (x = 0;x < strlen(pri->pvts[chanpos]->dialdest); x++) {
							ast_debug(1, "Sending pending digit '%c'\n", pri->pvts[chanpos]->dialdest[x]);
							pri_information(pri->pri, pri->pvts[chanpos]->call,
								pri->pvts[chanpos]->dialdest[x]);
						}
						ast_mutex_unlock(&pri->pvts[chanpos]->lock);
					} else
						ast_log(LOG_WARNING, "Unable to move channel %d!\n", e->setup_ack.channel);
				}
				break;
			case PRI_EVENT_NOTIFY:
				chanpos = pri_find_principle(pri, e->notify.channel);
				if (chanpos < 0) {
					ast_log(LOG_WARNING, "Received NOTIFY on unconfigured channel %d/%d span %d\n",
						PRI_SPAN(e->notify.channel), PRI_CHANNEL(e->notify.channel), pri->span);
				} else {
					struct ast_frame f = { AST_FRAME_CONTROL, };
					ast_mutex_lock(&pri->pvts[chanpos]->lock);
					switch (e->notify.info) {
					case PRI_NOTIFY_REMOTE_HOLD:
						f.subclass = AST_CONTROL_HOLD;
						dahdi_queue_frame(pri->pvts[chanpos], &f, pri);
						break;
					case PRI_NOTIFY_REMOTE_RETRIEVAL:
						f.subclass = AST_CONTROL_UNHOLD;
						dahdi_queue_frame(pri->pvts[chanpos], &f, pri);
						break;
					}
					ast_mutex_unlock(&pri->pvts[chanpos]->lock);
				}
				break;
			default:
				ast_debug(1, "Event: %d\n", e->e);
			}
		}
		ast_mutex_unlock(&pri->lock);
	}
	/* Never reached */
	return NULL;
}

static int start_pri(struct dahdi_pri *pri)
{
	int res, x;
	struct dahdi_params p;
	struct dahdi_bufferinfo bi;
	struct dahdi_spaninfo si;
	int i;

	for (i = 0; i < NUM_DCHANS; i++) {
		if (!pri->dchannels[i])
			break;
		pri->fds[i] = open("/dev/dahdi/channel", O_RDWR);
		x = pri->dchannels[i];
		if ((pri->fds[i] < 0) || (ioctl(pri->fds[i],DAHDI_SPECIFY,&x) == -1)) {
			ast_log(LOG_ERROR, "Unable to open D-channel %d (%s)\n", x, strerror(errno));
			return -1;
		}
		memset(&p, 0, sizeof(p));
		res = ioctl(pri->fds[i], DAHDI_GET_PARAMS, &p);
		if (res) {
			dahdi_close(pri->fds[i]);
			pri->fds[i] = -1;
			ast_log(LOG_ERROR, "Unable to get parameters for D-channel %d (%s)\n", x, strerror(errno));
			return -1;
		}
		if ((p.sigtype != DAHDI_SIG_HDLCFCS) && (p.sigtype != DAHDI_SIG_HARDHDLC)) {
			dahdi_close(pri->fds[i]);
			pri->fds[i] = -1;
			ast_log(LOG_ERROR, "D-channel %d is not in HDLC/FCS mode.\n", x);
			return -1;
		}
		memset(&si, 0, sizeof(si));
		res = ioctl(pri->fds[i], DAHDI_SPANSTAT, &si);
		if (res) {
			dahdi_close(pri->fds[i]);
			pri->fds[i] = -1;
			ast_log(LOG_ERROR, "Unable to get span state for D-channel %d (%s)\n", x, strerror(errno));
		}
		if (!si.alarms)
			pri->dchanavail[i] |= DCHAN_NOTINALARM;
		else
			pri->dchanavail[i] &= ~DCHAN_NOTINALARM;
		memset(&bi, 0, sizeof(bi));
		bi.txbufpolicy = DAHDI_POLICY_IMMEDIATE;
		bi.rxbufpolicy = DAHDI_POLICY_IMMEDIATE;
		bi.numbufs = 32;
		bi.bufsize = 1024;
		if (ioctl(pri->fds[i], DAHDI_SET_BUFINFO, &bi)) {
			ast_log(LOG_ERROR, "Unable to set appropriate buffering on channel %d: %s\n", x, strerror(errno));
			dahdi_close(pri->fds[i]);
			pri->fds[i] = -1;
			return -1;
		}
		switch (pri->sig) {
			case SIG_BRI:
				pri->dchans[i] = pri_new_bri(pri->fds[i], 1, pri->nodetype, pri->switchtype);
				break;
			case SIG_BRI_PTMP:
				pri->dchans[i] = pri_new_bri(pri->fds[i], 0, pri->nodetype, pri->switchtype);
				break;
			default:
				pri->dchans[i] = pri_new(pri->fds[i], pri->nodetype, pri->switchtype);
		}
		/* Force overlap dial if we're doing GR-303! */
		if (pri->switchtype == PRI_SWITCH_GR303_TMC)
			pri->overlapdial |= DAHDI_OVERLAPDIAL_BOTH;
		pri_set_overlapdial(pri->dchans[i],(pri->overlapdial & DAHDI_OVERLAPDIAL_OUTGOING)?1:0);
#ifdef HAVE_PRI_INBANDDISCONNECT
		pri_set_inbanddisconnect(pri->dchans[i], pri->inbanddisconnect);
#endif
		/* Enslave to master if appropriate */
		if (i)
			pri_enslave(pri->dchans[0], pri->dchans[i]);
		if (!pri->dchans[i]) {
			dahdi_close(pri->fds[i]);
			pri->fds[i] = -1;
			ast_log(LOG_ERROR, "Unable to create PRI structure\n");
			return -1;
		}
		pri_set_debug(pri->dchans[i], DEFAULT_PRI_DEBUG);
		pri_set_nsf(pri->dchans[i], pri->nsf);
#ifdef PRI_GETSET_TIMERS
		for (x = 0; x < PRI_MAX_TIMERS; x++) {
			if (pritimers[x] != 0)
				pri_set_timer(pri->dchans[i], x, pritimers[x]);
		}
#endif
	}
	/* Assume primary is the one we use */
	pri->pri = pri->dchans[0];
	pri->resetpos = -1;
	if (ast_pthread_create_background(&pri->master, NULL, pri_dchannel, pri)) {
		for (i = 0; i < NUM_DCHANS; i++) {
			if (!pri->dchannels[i])
				break;
			dahdi_close(pri->fds[i]);
			pri->fds[i] = -1;
		}
		ast_log(LOG_ERROR, "Unable to spawn D-channel: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

static char *complete_span_helper(const char *line, const char *word, int pos, int state, int rpos)
{
	int which, span;
	char *ret = NULL;

	if (pos != rpos)
		return ret;

	for (which = span = 0; span < NUM_SPANS; span++) {
		if (pris[span].pri && ++which > state) {
			asprintf(&ret, "%d", span + 1);	/* user indexes start from 1 */
			break;
		}
	}
	return ret;
}

static char *complete_span_4(const char *line, const char *word, int pos, int state)
{
	return complete_span_helper(line,word,pos,state,3);
}

static char *complete_span_5(const char *line, const char *word, int pos, int state)
{
	return complete_span_helper(line,word,pos,state,4);
}

static char *handle_pri_unset_debug_file(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "pri unset debug file";
		e->usage = "Usage: pri unset debug file\n"
			   "       Stop sending debug output to the previously \n"
		           "       specified file\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	/* Assume it is unset */
	ast_mutex_lock(&pridebugfdlock);
	close(pridebugfd);
	pridebugfd = -1;
	ast_cli(a->fd, "PRI debug output to file disabled\n");
	ast_mutex_unlock(&pridebugfdlock);
	return CLI_SUCCESS;
}

static char *handle_pri_set_debug_file(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int myfd;
	switch (cmd) {
	case CLI_INIT:
		e->command = "pri set debug file";
		e->usage = "Usage: pri set debug file [output-file]\n"
			   "       Sends PRI debug output to the specified output file\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (a->argc < 5)
		return CLI_SHOWUSAGE;

	if (ast_strlen_zero(a->argv[4]))
		return CLI_SHOWUSAGE;

	myfd = open(a->argv[4], O_CREAT|O_WRONLY, AST_FILE_MODE);
	if (myfd < 0) {
		ast_cli(a->fd, "Unable to open '%s' for writing\n", a->argv[4]);
		return CLI_SUCCESS;
	}

	ast_mutex_lock(&pridebugfdlock);

	if (pridebugfd >= 0)
		close(pridebugfd);

	pridebugfd = myfd;
	ast_copy_string(pridebugfilename,a->argv[4],sizeof(pridebugfilename));
	ast_mutex_unlock(&pridebugfdlock);
	ast_cli(a->fd, "PRI debug output will be sent to '%s'\n", a->argv[4]);
	return CLI_SUCCESS;
}

static char *handle_pri_debug(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int span;
	int x;
	switch (cmd) {
	case CLI_INIT:
		e->command = "pri debug span";
		e->usage =
			"Usage: pri debug span <span>\n"
			"       Enables debugging on a given PRI span\n";
		return NULL;
	case CLI_GENERATE:
		return complete_span_4(a->line, a->word, a->pos, a->n);
	}
	if (a->argc < 4) {
		return CLI_SHOWUSAGE;
	}
	span = atoi(a->argv[3]);
	if ((span < 1) || (span > NUM_SPANS)) {
		ast_cli(a->fd, "Invalid span %s.  Should be a number %d to %d\n", a->argv[3], 1, NUM_SPANS);
		return CLI_SUCCESS;
	}
	if (!pris[span-1].pri) {
		ast_cli(a->fd, "No PRI running on span %d\n", span);
		return CLI_SUCCESS;
	}
	for (x = 0; x < NUM_DCHANS; x++) {
		if (pris[span-1].dchans[x])
			pri_set_debug(pris[span-1].dchans[x], PRI_DEBUG_APDU |
			                                      PRI_DEBUG_Q931_DUMP | PRI_DEBUG_Q931_STATE |
			                                      PRI_DEBUG_Q921_STATE);
	}
	ast_cli(a->fd, "Enabled debugging on span %d\n", span);
	return CLI_SUCCESS;
}



static char *handle_pri_no_debug(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int span;
	int x;
	switch (cmd) {
	case CLI_INIT:
		e->command = "pri no debug span";
		e->usage =
			"Usage: pri no debug span <span>\n"
			"       Disables debugging on a given PRI span\n";
		return NULL;
	case CLI_GENERATE:
		return complete_span_5(a->line, a->word, a->pos, a->n);
	}
	if (a->argc < 5)
		return CLI_SHOWUSAGE;

	span = atoi(a->argv[4]);
	if ((span < 1) || (span > NUM_SPANS)) {
		ast_cli(a->fd, "Invalid span %s.  Should be a number %d to %d\n", a->argv[4], 1, NUM_SPANS);
		return CLI_SUCCESS;
	}
	if (!pris[span-1].pri) {
		ast_cli(a->fd, "No PRI running on span %d\n", span);
		return CLI_SUCCESS;
	}
	for (x = 0; x < NUM_DCHANS; x++) {
		if (pris[span-1].dchans[x])
			pri_set_debug(pris[span-1].dchans[x], 0);
	}
	ast_cli(a->fd, "Disabled debugging on span %d\n", span);
	return CLI_SUCCESS;
}

static char *handle_pri_really_debug(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int span;
	int x;
	switch (cmd) {
	case CLI_INIT:
		e->command = "pri intensive debug span";
		e->usage =
			"Usage: pri intensive debug span <span>\n"
			"       Enables debugging down to the Q.921 level\n";
		return NULL;
	case CLI_GENERATE:
		return complete_span_5(a->line, a->word, a->pos, a->n);
	}

	if (a->argc < 5)
		return CLI_SHOWUSAGE;
	span = atoi(a->argv[4]);
	if ((span < 1) || (span > NUM_SPANS)) {
		ast_cli(a->fd, "Invalid span %s.  Should be a number %d to %d\n", a->argv[4], 1, NUM_SPANS);
		return CLI_SUCCESS;
	}
	if (!pris[span-1].pri) {
		ast_cli(a->fd, "No PRI running on span %d\n", span);
		return CLI_SUCCESS;
	}
	for (x = 0; x < NUM_DCHANS; x++) {
		if (pris[span-1].dchans[x])
			pri_set_debug(pris[span-1].dchans[x], PRI_DEBUG_APDU |
			                                      PRI_DEBUG_Q931_DUMP | PRI_DEBUG_Q931_STATE |
			                                      PRI_DEBUG_Q921_RAW | PRI_DEBUG_Q921_DUMP | PRI_DEBUG_Q921_STATE);
	}
	ast_cli(a->fd, "Enabled EXTENSIVE debugging on span %d\n", span);
	return CLI_SUCCESS;
}

static void build_status(char *s, size_t len, int status, int active)
{
	if (!s || len < 1) {
		return;
	}
	s[0] = '\0';
	if (status & DCHAN_PROVISIONED)
		strncat(s, "Provisioned, ", len - strlen(s) - 1);
	if (!(status & DCHAN_NOTINALARM))
		strncat(s, "In Alarm, ", len - strlen(s) - 1);
	if (status & DCHAN_UP)
		strncat(s, "Up", len - strlen(s) - 1);
	else
		strncat(s, "Down", len - strlen(s) - 1);
	if (active)
		strncat(s, ", Active", len - strlen(s) - 1);
	else
		strncat(s, ", Standby", len - strlen(s) - 1);
	s[len - 1] = '\0';
}

static char *handle_pri_show_spans(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int span;
	int x;
	char status[256];

	switch (cmd) {
	case CLI_INIT:
		e->command = "pri show spans";
		e->usage =
			"Usage: pri show spans\n"
			"       Displays PRI Information\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 3)
		return CLI_SHOWUSAGE;

	for (span = 0; span < NUM_SPANS; span++) {
		if (pris[span].pri) {
			for (x = 0; x < NUM_DCHANS; x++) {
				if (pris[span].dchannels[x]) {
					build_status(status, sizeof(status), pris[span].dchanavail[x], pris[span].dchans[x] == pris[span].pri);
					ast_cli(a->fd, "PRI span %d/%d: %s\n", span + 1, x, status);
				}
			}
		}
	}
	return CLI_SUCCESS;
}

static char *handle_pri_show_span(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int span;
	int x;
	char status[256];
	switch (cmd) {
	case CLI_INIT:
		e->command = "pri show span";
		e->usage =
			"Usage: pri show span <span>\n"
			"       Displays PRI Information on a given PRI span\n";
		return NULL;
	case CLI_GENERATE:
		return complete_span_4(a->line, a->word, a->pos, a->n);
	}

	if (a->argc < 4)
		return CLI_SHOWUSAGE;
	span = atoi(a->argv[3]);
	if ((span < 1) || (span > NUM_SPANS)) {
		ast_cli(a->fd, "Invalid span '%s'.  Should be a number from %d to %d\n", a->argv[3], 1, NUM_SPANS);
		return CLI_SUCCESS;
	}
	if (!pris[span-1].pri) {
		ast_cli(a->fd, "No PRI running on span %d\n", span);
		return CLI_SUCCESS;
	}
	for (x = 0; x < NUM_DCHANS; x++) {
		if (pris[span-1].dchannels[x]) {
#ifdef PRI_DUMP_INFO_STR
			char *info_str = NULL;
#endif
			ast_cli(a->fd, "%s D-channel: %d\n", pri_order(x), pris[span-1].dchannels[x]);
			build_status(status, sizeof(status), pris[span-1].dchanavail[x], pris[span-1].dchans[x] == pris[span-1].pri);
			ast_cli(a->fd, "Status: %s\n", status);
#ifdef PRI_DUMP_INFO_STR
			info_str = pri_dump_info_str(pris[span-1].pri);
			if (info_str) {
				ast_cli(a->fd, "%s", info_str);
				ast_free(info_str);
			}
#else
			pri_dump_info(pris[span-1].pri);
#endif
			ast_cli(a->fd, "Overlap Recv: %s\n\n", (pris[span-1].overlapdial & DAHDI_OVERLAPDIAL_INCOMING)?"Yes":"No");
		}
	}
	return CLI_SUCCESS;
}

static char *handle_pri_show_debug(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int x;
	int span;
	int count=0;
	int debug=0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "pri show debug";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	for (span = 0; span < NUM_SPANS; span++) {
	        if (pris[span].pri) {
			for (x = 0; x < NUM_DCHANS; x++) {
				debug = 0;
	        		if (pris[span].dchans[x]) {
	        			debug = pri_get_debug(pris[span].dchans[x]);
					ast_cli(a->fd, "Span %d: Debug: %s\tIntense: %s\n", span+1, (debug&PRI_DEBUG_Q931_STATE)? "Yes" : "No" ,(debug&PRI_DEBUG_Q921_RAW)? "Yes" : "No" );
					count++;
				}
			}
		}

	}
	ast_mutex_lock(&pridebugfdlock);
	if (pridebugfd >= 0)
		ast_cli(a->fd, "Logging PRI debug to file %s\n", pridebugfilename);
	ast_mutex_unlock(&pridebugfdlock);

	if (!count)
		ast_cli(a->fd, "No debug set or no PRI running\n");
	return CLI_SUCCESS;
}

static struct ast_cli_entry dahdi_pri_cli[] = {
	AST_CLI_DEFINE(handle_pri_debug, "Enables PRI debugging on a span"),
	AST_CLI_DEFINE(handle_pri_no_debug, "Disables PRI debugging on a span"),
	AST_CLI_DEFINE(handle_pri_really_debug, "Enables REALLY INTENSE PRI debugging"),
	AST_CLI_DEFINE(handle_pri_show_spans, "Displays PRI Information"),
	AST_CLI_DEFINE(handle_pri_show_span, "Displays PRI Information"),
	AST_CLI_DEFINE(handle_pri_show_debug, "Displays current PRI debug settings"),
	AST_CLI_DEFINE(handle_pri_set_debug_file, "Sends PRI debug output to the specified file"),
	AST_CLI_DEFINE(handle_pri_unset_debug_file, "Ends PRI debug output to file"),
};

#endif /* HAVE_PRI */

static char *dahdi_destroy_channel(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int channel;
	int ret;
	switch (cmd) {
	case CLI_INIT:
		e->command = "dahdi destroy channel";
		e->usage =
			"Usage: dahdi destroy channel <chan num>\n"
			"	DON'T USE THIS UNLESS YOU KNOW WHAT YOU ARE DOING.  Immediately removes a given channel, whether it is in use or not\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (a->argc != 4)
		return CLI_SHOWUSAGE;

	channel = atoi(a->argv[3]);
	ret = dahdi_destroy_channel_bynum(channel);
	return ( RESULT_SUCCESS == ret ) ? CLI_SUCCESS : CLI_FAILURE;
}

static void dahdi_softhangup_all(void)
{
	struct dahdi_pvt *p;
retry:
	ast_mutex_lock(&iflock);
    for (p = iflist; p; p = p->next) {
		ast_mutex_lock(&p->lock);
        if (p->owner && !p->restartpending) {
			if (ast_channel_trylock(p->owner)) {
				if (option_debug > 2)
					ast_verbose("Avoiding deadlock\n");
				/* Avoid deadlock since you're not supposed to lock iflock or pvt before a channel */
				ast_mutex_unlock(&p->lock);
				ast_mutex_unlock(&iflock);
				goto retry;
			}
			if (option_debug > 2)
				ast_verbose("Softhanging up on %s\n", p->owner->name);
			ast_softhangup_nolock(p->owner, AST_SOFTHANGUP_EXPLICIT);
			p->restartpending = 1;
			num_restart_pending++;
			ast_channel_unlock(p->owner);
		}
		ast_mutex_unlock(&p->lock);
    }
	ast_mutex_unlock(&iflock);
}

static int setup_dahdi(int reload);
static int dahdi_restart(void)
{
#if defined(HAVE_PRI) || defined(HAVE_SS7)
	int i, j;
#endif
	int cancel_code;
	struct dahdi_pvt *p;

	ast_mutex_lock(&restart_lock);

	ast_verb(1, "Destroying channels and reloading DAHDI configuration.\n");
	dahdi_softhangup_all();
	ast_verb(4, "Initial softhangup of all DAHDI channels complete.\n");

#if defined(HAVE_PRI)
	for (i = 0; i < NUM_SPANS; i++) {
		if (pris[i].master && (pris[i].master != AST_PTHREADT_NULL)) {
			cancel_code = pthread_cancel(pris[i].master);
			pthread_kill(pris[i].master, SIGURG);
			ast_debug(4, "Waiting to join thread of span %d with pid=%p, cancel_code=%d\n", i, (void *) pris[i].master, cancel_code);
			pthread_join(pris[i].master, NULL);
			ast_debug(4, "Joined thread of span %d\n", i);
		}
	}
#endif

#if defined(HAVE_SS7)
	for (i = 0; i < NUM_SPANS; i++) {
		if (linksets[i].master && (linksets[i].master != AST_PTHREADT_NULL)) {
			cancel_code = pthread_cancel(linksets[i].master);
			pthread_kill(linksets[i].master, SIGURG);
			ast_debug(4, "Waiting to join thread of span %d with pid=%p, cancel_code=%d\n", i, (void *) linksets[i].master, cancel_code);
			pthread_join(linksets[i].master, NULL);
			ast_debug(4, "Joined thread of span %d\n", i);
		}
    }
#endif

	ast_mutex_lock(&monlock);
	if (monitor_thread && (monitor_thread != AST_PTHREADT_STOP) && (monitor_thread != AST_PTHREADT_NULL)) {
		cancel_code = pthread_cancel(monitor_thread);
		pthread_kill(monitor_thread, SIGURG);
		ast_debug(4, "Waiting to join monitor thread with pid=%p, cancel_code=%d\n", (void *) monitor_thread, cancel_code);
		pthread_join(monitor_thread, NULL);
		ast_debug(4, "Joined monitor thread\n");
	}
	monitor_thread = AST_PTHREADT_NULL; /* prepare to restart thread in setup_dahdi once channels are reconfigured */

	ast_mutex_lock(&ss_thread_lock);
	while (ss_thread_count > 0) { /* let ss_threads finish and run dahdi_hangup before dahvi_pvts are destroyed */
		int x = DAHDI_FLASH;
		ast_debug(3, "Waiting on %d ss_thread(s) to finish\n", ss_thread_count);

		for (p = iflist; p; p = p->next) {
			if (p->owner)
				ioctl(p->subs[SUB_REAL].dfd, DAHDI_HOOK, &x); /* important to create an event for dahdi_wait_event to register so that all ss_threads terminate */
			}
			ast_cond_wait(&ss_thread_complete, &ss_thread_lock);
		}

	/* ensure any created channels before monitor threads were stopped are hungup */
	dahdi_softhangup_all();
	ast_verb(4, "Final softhangup of all DAHDI channels complete.\n");
	destroy_all_channels();
	ast_debug(1, "Channels destroyed. Now re-reading config. %d active channels remaining.\n", ast_active_channels());

	ast_mutex_unlock(&monlock);

#ifdef HAVE_PRI
	for (i = 0; i < NUM_SPANS; i++) {
		for (j = 0; j < NUM_DCHANS; j++)
			dahdi_close(pris[i].fds[j]);
	}

	memset(pris, 0, sizeof(pris));
	for (i = 0; i < NUM_SPANS; i++) {
		ast_mutex_init(&pris[i].lock);
		pris[i].offset = -1;
		pris[i].master = AST_PTHREADT_NULL;
		for (j = 0; j < NUM_DCHANS; j++)
			pris[i].fds[j] = -1;
		}
	pri_set_error(dahdi_pri_error);
	pri_set_message(dahdi_pri_message);
#endif
#ifdef HAVE_SS7
	for (i = 0; i < NUM_SPANS; i++) {
		for (j = 0; j < NUM_DCHANS; j++)
			dahdi_close(linksets[i].fds[j]);
	}

	memset(linksets, 0, sizeof(linksets));
	for (i = 0; i < NUM_SPANS; i++) {
		ast_mutex_init(&linksets[i].lock);
		linksets[i].master = AST_PTHREADT_NULL;
		for (j = 0; j < NUM_DCHANS; j++)
			linksets[i].fds[j] = -1;
	}
	ss7_set_error(dahdi_ss7_error);
	ss7_set_message(dahdi_ss7_message);
	ss7_set_hangup(dahdi_ss7_hangup);
	ss7_set_notinservice(dahdi_ss7_notinservice);
	ss7_set_call_null(dahdi_ss7_call_null);
#endif

	if (setup_dahdi(2) != 0) {
		ast_log(LOG_WARNING, "Reload channels from dahdi config failed!\n");
		ast_mutex_unlock(&ss_thread_lock);
		return 1;
	}
	ast_mutex_unlock(&ss_thread_lock);
	ast_mutex_unlock(&restart_lock);
	return 0;
}

static char *dahdi_restart_cmd(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "dahdi restart";
		e->usage =
			"Usage: dahdi restart\n"
			"	Restarts the DAHDI channels: destroys them all and then\n"
			"	re-reads them from chan_dahdi.conf.\n"
			"	Note that this will STOP any running CALL on DAHDI channels.\n"
			"";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (a->argc != 2)
		return CLI_SHOWUSAGE;

	if (dahdi_restart() != 0)
		return CLI_FAILURE;
	return CLI_SUCCESS;
}

static int action_dahdirestart(struct mansession *s, const struct message *m)
{
	if (dahdi_restart() != 0) {
		astman_send_error(s, m, "Failed rereading DAHDI configuration");
		return 1;
	}
	astman_send_ack(s, m, "DAHDIRestart: Success");
	return 0;
}

static char *dahdi_show_channels(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
#define FORMAT "%7s %-10.10s %-15.15s %-10.10s %-20.20s %-10.10s %-10.10s\n"
#define FORMAT2 "%7s %-10.10s %-15.15s %-10.10s %-20.20s %-10.10s %-10.10s\n"
	unsigned int targetnum = 0;
	int filtertype = 0;
	struct dahdi_pvt *tmp = NULL;
	char tmps[20] = "";
	char statestr[20] = "";
	char blockstr[20] = "";
	ast_mutex_t *lock;
	struct dahdi_pvt *start;
#ifdef HAVE_PRI
	int trunkgroup;
	struct dahdi_pri *pri = NULL;
	int x;
#endif
	switch (cmd) {
	case CLI_INIT:
		e->command = "dahdi show channels [trunkgroup|group|context]";
		e->usage =
			"Usage: dahdi show channels [ trunkgroup <trunkgroup> | group <group> | context <context> ]\n"
			"	Shows a list of available channels with optional filtering\n"
			"	<group> must be a number between 0 and 63\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	lock = &iflock;
	start = iflist;

	/* syntax: dahdi show channels [ group <group> | context <context> | trunkgroup <trunkgroup> ] */

	if (!((a->argc == 3) || (a->argc == 5)))
		return CLI_SHOWUSAGE;

	if (a->argc == 5) {
#ifdef HAVE_PRI
		if (!strcasecmp(a->argv[3], "trunkgroup")) {
			/* this option requires no special handling, so leave filtertype to zero */
			if ((trunkgroup = atoi(a->argv[4])) < 1)
				return CLI_SHOWUSAGE;
			for (x = 0; x < NUM_SPANS; x++) {
				if (pris[x].trunkgroup == trunkgroup) {
					pri = pris + x;
					break;
				}
			}
			if (pri) {
				start = pri->crvs;
				lock = &pri->lock;
			} else {
				ast_cli(a->fd, "No such trunk group %d\n", trunkgroup);
				return CLI_FAILURE;
			}
		} else
#endif
		if (!strcasecmp(a->argv[3], "group")) {
			targetnum = atoi(a->argv[4]);
			if ((targetnum < 0) || (targetnum > 63))
				return CLI_SHOWUSAGE;
			targetnum = 1 << targetnum;
			filtertype = 1;
		} else if (!strcasecmp(a->argv[3], "context")) {
			filtertype = 2;
		}
	}

	ast_mutex_lock(lock);
#ifdef HAVE_PRI
	ast_cli(a->fd, FORMAT2, pri ? "CRV" : "Chan", "Extension", "Context", "Language", "MOH Interpret", "Blocked", "State");
#else
	ast_cli(a->fd, FORMAT2, "Chan", "Extension", "Context", "Language", "MOH Interpret", "Blocked", "State");
#endif

	tmp = start;
	while (tmp) {
		if (filtertype) {
			switch(filtertype) {
			case 1: /* dahdi show channels group <group> */
				if (tmp->group != targetnum) {
					tmp = tmp->next;
					continue;
				}
				break;
			case 2: /* dahdi show channels context <context> */
				if (strcasecmp(tmp->context, a->argv[4])) {
					tmp = tmp->next;
					continue;
				}
				break;
			default:
				;
			}
		}
		if (tmp->channel > 0) {
			snprintf(tmps, sizeof(tmps), "%d", tmp->channel);
		} else
			ast_copy_string(tmps, "pseudo", sizeof(tmps));

		if (tmp->locallyblocked)
			blockstr[0] = 'L';
		else
			blockstr[0] = ' ';

		if (tmp->remotelyblocked)
			blockstr[1] = 'R';
		else
			blockstr[1] = ' ';

		blockstr[2] = '\0';

		snprintf(statestr, sizeof(statestr), "%s", tmp->inservice ? "In Service" : "Not In Service");

		ast_cli(a->fd, FORMAT, tmps, tmp->exten, tmp->context, tmp->language, tmp->mohinterpret, blockstr, statestr);
		tmp = tmp->next;
	}
	ast_mutex_unlock(lock);
	return CLI_SUCCESS;
#undef FORMAT
#undef FORMAT2
}

static char *dahdi_show_channel(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int channel;
	struct dahdi_pvt *tmp = NULL;
	struct dahdi_confinfo ci;
	struct dahdi_params ps;
	int x;
	ast_mutex_t *lock;
	struct dahdi_pvt *start;
#ifdef HAVE_PRI
	char *c;
	int trunkgroup;
	struct dahdi_pri *pri=NULL;
#endif
	switch (cmd) {
	case CLI_INIT:
		e->command = "dahdi show channel";
		e->usage =
			"Usage: dahdi show channel <chan num>\n"
			"	Detailed information about a given channel\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	lock = &iflock;
	start = iflist;

	if (a->argc != 4)
		return CLI_SHOWUSAGE;
#ifdef HAVE_PRI
	if ((c = strchr(a->argv[3], ':'))) {
		if (sscanf(a->argv[3], "%d:%d", &trunkgroup, &channel) != 2)
			return CLI_SHOWUSAGE;
		if ((trunkgroup < 1) || (channel < 1))
			return CLI_SHOWUSAGE;
		for (x = 0; x < NUM_SPANS; x++) {
			if (pris[x].trunkgroup == trunkgroup) {
				pri = pris + x;
				break;
			}
		}
		if (pri) {
			start = pri->crvs;
			lock = &pri->lock;
		} else {
			ast_cli(a->fd, "No such trunk group %d\n", trunkgroup);
			return CLI_FAILURE;
		}
	} else
#endif
		channel = atoi(a->argv[3]);

	ast_mutex_lock(lock);
	tmp = start;
	while (tmp) {
		if (tmp->channel == channel) {
#ifdef HAVE_PRI
			if (pri)
				ast_cli(a->fd, "Trunk/CRV: %d/%d\n", trunkgroup, tmp->channel);
			else
#endif
			ast_cli(a->fd, "Channel: %d\n", tmp->channel);
			ast_cli(a->fd, "File Descriptor: %d\n", tmp->subs[SUB_REAL].dfd);
			ast_cli(a->fd, "Span: %d\n", tmp->span);
			ast_cli(a->fd, "Extension: %s\n", tmp->exten);
			ast_cli(a->fd, "Dialing: %s\n", tmp->dialing ? "yes" : "no");
			ast_cli(a->fd, "Context: %s\n", tmp->context);
			ast_cli(a->fd, "Caller ID: %s\n", tmp->cid_num);
			ast_cli(a->fd, "Calling TON: %d\n", tmp->cid_ton);
			ast_cli(a->fd, "Caller ID name: %s\n", tmp->cid_name);
			ast_cli(a->fd, "Mailbox: %s\n", S_OR(tmp->mailbox, "none"));
			if (tmp->vars) {
				struct ast_variable *v;
				ast_cli(a->fd, "Variables:\n");
				for (v = tmp->vars ; v ; v = v->next)
					ast_cli(a->fd, "       %s = %s\n", v->name, v->value);
			}
			ast_cli(a->fd, "Destroy: %d\n", tmp->destroy);
			ast_cli(a->fd, "InAlarm: %d\n", tmp->inalarm);
			ast_cli(a->fd, "Signalling Type: %s\n", sig2str(tmp->sig));
			ast_cli(a->fd, "Radio: %d\n", tmp->radio);
			ast_cli(a->fd, "Owner: %s\n", tmp->owner ? tmp->owner->name : "<None>");
			ast_cli(a->fd, "Real: %s%s%s\n", tmp->subs[SUB_REAL].owner ? tmp->subs[SUB_REAL].owner->name : "<None>", tmp->subs[SUB_REAL].inthreeway ? " (Confed)" : "", tmp->subs[SUB_REAL].linear ? " (Linear)" : "");
			ast_cli(a->fd, "Callwait: %s%s%s\n", tmp->subs[SUB_CALLWAIT].owner ? tmp->subs[SUB_CALLWAIT].owner->name : "<None>", tmp->subs[SUB_CALLWAIT].inthreeway ? " (Confed)" : "", tmp->subs[SUB_CALLWAIT].linear ? " (Linear)" : "");
			ast_cli(a->fd, "Threeway: %s%s%s\n", tmp->subs[SUB_THREEWAY].owner ? tmp->subs[SUB_THREEWAY].owner->name : "<None>", tmp->subs[SUB_THREEWAY].inthreeway ? " (Confed)" : "", tmp->subs[SUB_THREEWAY].linear ? " (Linear)" : "");
			ast_cli(a->fd, "Confno: %d\n", tmp->confno);
			ast_cli(a->fd, "Propagated Conference: %d\n", tmp->propconfno);
			ast_cli(a->fd, "Real in conference: %d\n", tmp->inconference);
			ast_cli(a->fd, "DSP: %s\n", tmp->dsp ? "yes" : "no");
			ast_cli(a->fd, "Busy Detection: %s\n", tmp->busydetect ? "yes" : "no");
			if (tmp->busydetect) {
#if defined(BUSYDETECT_TONEONLY)
				ast_cli(a->fd, "    Busy Detector Helper: BUSYDETECT_TONEONLY\n");
#elif defined(BUSYDETECT_COMPARE_TONE_AND_SILENCE)
				ast_cli(a->fd, "    Busy Detector Helper: BUSYDETECT_COMPARE_TONE_AND_SILENCE\n");
#endif
#ifdef BUSYDETECT_DEBUG
				ast_cli(a->fd, "    Busy Detector Debug: Enabled\n");
#endif
				ast_cli(a->fd, "    Busy Count: %d\n", tmp->busycount);
				ast_cli(a->fd, "    Busy Pattern: %d,%d\n", tmp->busy_tonelength, tmp->busy_quietlength);
			}
			ast_cli(a->fd, "TDD: %s\n", tmp->tdd ? "yes" : "no");
			ast_cli(a->fd, "Relax DTMF: %s\n", tmp->dtmfrelax ? "yes" : "no");
			ast_cli(a->fd, "Dialing/CallwaitCAS: %d/%d\n", tmp->dialing, tmp->callwaitcas);
			ast_cli(a->fd, "Default law: %s\n", tmp->law == DAHDI_LAW_MULAW ? "ulaw" : tmp->law == DAHDI_LAW_ALAW ? "alaw" : "unknown");
			ast_cli(a->fd, "Fax Handled: %s\n", tmp->faxhandled ? "yes" : "no");
			ast_cli(a->fd, "Pulse phone: %s\n", tmp->pulsedial ? "yes" : "no");
			ast_cli(a->fd, "DND: %s\n", tmp->dnd ? "yes" : "no");
			ast_cli(a->fd, "Echo Cancellation:\n");
			if (tmp->echocancel.head.tap_length) {
				ast_cli(a->fd, "\t%d taps\n", tmp->echocancel.head.tap_length);
				for (x = 0; x < tmp->echocancel.head.param_count; x++) {
					ast_cli(a->fd, "\t\t%s: %ud\n", tmp->echocancel.params[x].name, tmp->echocancel.params[x].value);
				}
				ast_cli(a->fd, "\t%scurrently %s\n", tmp->echocanbridged ? "" : "(unless TDM bridged) ", tmp->echocanon ? "ON" : "OFF");
			} else {
				ast_cli(a->fd, "\tnone\n");
			}
			if (tmp->master)
				ast_cli(a->fd, "Master Channel: %d\n", tmp->master->channel);
			for (x = 0; x < MAX_SLAVES; x++) {
				if (tmp->slaves[x])
					ast_cli(a->fd, "Slave Channel: %d\n", tmp->slaves[x]->channel);
			}
#ifdef HAVE_SS7
			if (tmp->ss7) {
				ast_cli(a->fd, "CIC: %d\n", tmp->cic);
			}
#endif
#ifdef HAVE_PRI
			if (tmp->pri) {
				ast_cli(a->fd, "PRI Flags: ");
				if (tmp->resetting)
					ast_cli(a->fd, "Resetting ");
				if (tmp->call)
					ast_cli(a->fd, "Call ");
				if (tmp->bearer)
					ast_cli(a->fd, "Bearer ");
				ast_cli(a->fd, "\n");
				if (tmp->logicalspan)
					ast_cli(a->fd, "PRI Logical Span: %d\n", tmp->logicalspan);
				else
					ast_cli(a->fd, "PRI Logical Span: Implicit\n");
			}

#endif
			memset(&ci, 0, sizeof(ci));
			ps.channo = tmp->channel;
			if (tmp->subs[SUB_REAL].dfd > -1) {
				memset(&ci, 0, sizeof(ci));
				if (!ioctl(tmp->subs[SUB_REAL].dfd, DAHDI_GETCONF, &ci)) {
					ast_cli(a->fd, "Actual Confinfo: Num/%d, Mode/0x%04x\n", ci.confno, ci.confmode);
				}
				if (!ioctl(tmp->subs[SUB_REAL].dfd, DAHDI_GETCONFMUTE, &x)) {
					ast_cli(a->fd, "Actual Confmute: %s\n", x ? "Yes" : "No");
				}
				memset(&ps, 0, sizeof(ps));
				if (ioctl(tmp->subs[SUB_REAL].dfd, DAHDI_GET_PARAMS, &ps) < 0) {
					ast_log(LOG_WARNING, "Failed to get parameters on channel %d: %s\n", tmp->channel, strerror(errno));
				} else {
					ast_cli(a->fd, "Hookstate (FXS only): %s\n", ps.rxisoffhook ? "Offhook" : "Onhook");
				}
			}
			ast_mutex_unlock(lock);
			return CLI_SUCCESS;
		}
		tmp = tmp->next;
	}

	ast_cli(a->fd, "Unable to find given channel %d\n", channel);
	ast_mutex_unlock(lock);
	return CLI_FAILURE;
}

static char *handle_dahdi_show_cadences(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int i, j;
	switch (cmd) {
	case CLI_INIT:
		e->command = "dahdi show cadences";
		e->usage =
			"Usage: dahdi show cadences\n"
			"       Shows all cadences currently defined\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	for (i = 0; i < num_cadence; i++) {
		char output[1024];
		char tmp[16], tmp2[64];
		snprintf(tmp, sizeof(tmp), "r%d: ", i + 1);
		term_color(output, tmp, COLOR_GREEN, COLOR_BLACK, sizeof(output));

		for (j = 0; j < 16; j++) {
			if (cadences[i].ringcadence[j] == 0)
				break;
			snprintf(tmp, sizeof(tmp), "%d", cadences[i].ringcadence[j]);
			if (cidrings[i] * 2 - 1 == j)
				term_color(tmp2, tmp, COLOR_MAGENTA, COLOR_BLACK, sizeof(tmp2) - 1);
			else
				term_color(tmp2, tmp, COLOR_GREEN, COLOR_BLACK, sizeof(tmp2) - 1);
			if (j != 0)
				strncat(output, ",", sizeof(output) - strlen(output) - 1);
			strncat(output, tmp2, sizeof(output) - strlen(output) - 1);
		}
		ast_cli(a->fd,"%s\n",output);
	}
	return CLI_SUCCESS;
}

/* Based on irqmiss.c */
static char *dahdi_show_status(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	#define FORMAT "%-40.40s %-7.7s %-6d %-6d %-6d %-3.3s %-4.4s %-8.8s %s\n"
	#define FORMAT2 "%-40.40s %-7.7s %-6.6s %-6.6s %-6.6s %-3.3s %-4.4s %-8.8s %s\n"
	int span;
	int res;
	char alarms[50];

	int ctl;
	struct dahdi_spaninfo s;

	switch (cmd) {
	case CLI_INIT:
		e->command = "dahdi show status";
		e->usage =
			"Usage: dahdi show status\n"
			"       Shows a list of DAHDI cards with status\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	ctl = open("/dev/dahdi/ctl", O_RDWR);
	if (ctl < 0) {
		ast_cli(a->fd, "No DAHDI found. Unable to open /dev/dahdi/ctl: %s\n", strerror(errno));
		return CLI_FAILURE;
	}
	ast_cli(a->fd, FORMAT2, "Description", "Alarms", "IRQ", "bpviol", "CRC4", "Framing", "Coding", "Options", "LBO");

	for (span = 1; span < DAHDI_MAX_SPANS; ++span) {
		s.spanno = span;
		res = ioctl(ctl, DAHDI_SPANSTAT, &s);
		if (res) {
			continue;
		}
		alarms[0] = '\0';
		if (s.alarms > 0) {
			if (s.alarms & DAHDI_ALARM_BLUE)
				strcat(alarms, "BLU/");
			if (s.alarms & DAHDI_ALARM_YELLOW)
				strcat(alarms, "YEL/");
			if (s.alarms & DAHDI_ALARM_RED)
				strcat(alarms, "RED/");
			if (s.alarms & DAHDI_ALARM_LOOPBACK)
				strcat(alarms, "LB/");
			if (s.alarms & DAHDI_ALARM_RECOVER)
				strcat(alarms, "REC/");
			if (s.alarms & DAHDI_ALARM_NOTOPEN)
				strcat(alarms, "NOP/");
			if (!strlen(alarms))
				strcat(alarms, "UUU/");
			if (strlen(alarms)) {
				/* Strip trailing / */
				alarms[strlen(alarms) - 1] = '\0';
			}
		} else {
			if (s.numchans)
				strcpy(alarms, "OK");
			else
				strcpy(alarms, "UNCONFIGURED");
		}

		ast_cli(a->fd, FORMAT, s.desc, alarms, s.irqmisses, s.bpvcount, s.crc4count
				, s.lineconfig & DAHDI_CONFIG_D4 ? "D4" :
				  s.lineconfig & DAHDI_CONFIG_ESF ? "ESF" :
				  s.lineconfig & DAHDI_CONFIG_CCS ? "CCS" :
				  "CAS"
				, s.lineconfig & DAHDI_CONFIG_B8ZS ? "B8ZS" :
				  s.lineconfig & DAHDI_CONFIG_HDB3 ? "HDB3" :
				  s.lineconfig & DAHDI_CONFIG_AMI ? "AMI" :
				  "Unk"
				, s.lineconfig & DAHDI_CONFIG_CRC4 ?
				  s.lineconfig & DAHDI_CONFIG_NOTOPEN ? "CRC4/YEL" : "CRC4" : "YEL"
				, lbostr[s.lbo]
			);
	}
	close(ctl);

	return CLI_SUCCESS;
#undef FORMAT
#undef FORMAT2
}

static char *dahdi_show_version(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int pseudo_fd = -1;
	struct dahdi_versioninfo vi;

	switch (cmd) {
	case CLI_INIT:
		e->command = "dahdi show version";
		e->usage =
			"Usage: dahdi show version\n"
			"       Shows the DAHDI version in use\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if ((pseudo_fd = open("/dev/dahdi/ctl", O_RDONLY)) < 0) {
		ast_cli(a->fd, "Failed to open control file to get version.\n");
		return CLI_SUCCESS;
	}

	strcpy(vi.version, "Unknown");
	strcpy(vi.echo_canceller, "Unknown");

	if (ioctl(pseudo_fd, DAHDI_GETVERSION, &vi))
		ast_cli(a->fd, "Failed to get DAHDI version: %s\n", strerror(errno));
	else
		ast_cli(a->fd, "DAHDI Version: %s Echo Canceller: %s\n", vi.version, vi.echo_canceller);

	close(pseudo_fd);

	return CLI_SUCCESS;
}

static char *dahdi_set_hwgain(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int channel;
	int gain;
	int tx;
	struct dahdi_hwgain hwgain;
	struct dahdi_pvt *tmp = NULL;

	switch (cmd) {
	case CLI_INIT:
		e->command = "dahdi set hwgain";
		e->usage =
			"Usage: dahdi set hwgain <rx|tx> <chan#> <gain>\n"
			"	Sets the hardware gain on a a given channel, overriding the\n"
			"   value provided at module loadtime, whether the channel is in\n"
			"   use or not.  Changes take effect immediately.\n"
			"   <rx|tx> which direction do you want to change (relative to our module)\n"
			"   <chan num> is the channel number relative to the device\n"
			"   <gain> is the gain in dB (e.g. -3.5 for -3.5dB)\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 6)
		return CLI_SHOWUSAGE;

	if (!strcasecmp("rx", a->argv[3]))
		tx = 0; /* rx */
	else if (!strcasecmp("tx", a->argv[3]))
		tx = 1; /* tx */
	else
		return CLI_SHOWUSAGE;

	channel = atoi(a->argv[4]);
	gain = atof(a->argv[5])*10.0;

	ast_mutex_lock(&iflock);

	for (tmp = iflist; tmp; tmp = tmp->next) {

		if (tmp->channel != channel)
			continue;

		if (tmp->subs[SUB_REAL].dfd == -1)
			break;

		hwgain.newgain = gain;
		hwgain.tx = tx;
		if (ioctl(tmp->subs[SUB_REAL].dfd, DAHDI_SET_HWGAIN, &hwgain) < 0) {
			ast_cli(a->fd, "Unable to set the hardware gain for channel %d: %s\n", channel, strerror(errno));
			ast_mutex_unlock(&iflock);
			return CLI_FAILURE;
		}
		ast_cli(a->fd, "hardware %s gain set to %d (%.1f dB) on channel %d\n",
			tx ? "tx" : "rx", gain, (float)gain/10.0, channel);
		break;
	}

	ast_mutex_unlock(&iflock);

	if (tmp)
		return CLI_SUCCESS;

	ast_cli(a->fd, "Unable to find given channel %d\n", channel);
	return CLI_FAILURE;

}

static char *dahdi_set_swgain(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int channel;
	float gain;
	int tx;
	int res;
	ast_mutex_t *lock;
	struct dahdi_pvt *tmp = NULL;

	switch (cmd) {
	case CLI_INIT:
		e->command = "dahdi set swgain";
		e->usage =
			"Usage: dahdi set swgain <rx|tx> <chan#> <gain>\n"
			"	Sets the software gain on a a given channel, overriding the\n"
			"   value provided at module loadtime, whether the channel is in\n"
			"   use or not.  Changes take effect immediately.\n"
			"   <rx|tx> which direction do you want to change (relative to our module)\n"
			"   <chan num> is the channel number relative to the device\n"
			"   <gain> is the gain in dB (e.g. -3.5 for -3.5dB)\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	lock = &iflock;

	if (a->argc != 6)
		return CLI_SHOWUSAGE;

	if (!strcasecmp("rx", a->argv[3]))
		tx = 0; /* rx */
	else if (!strcasecmp("tx", a->argv[3]))
		tx = 1; /* tx */
	else
		return CLI_SHOWUSAGE;

	channel = atoi(a->argv[4]);
	gain = atof(a->argv[5]);

	ast_mutex_lock(lock);
	for (tmp = iflist; tmp; tmp = tmp->next) {

		if (tmp->channel != channel)
			continue;

		if (tmp->subs[SUB_REAL].dfd == -1)
			break;

		if (tx)
			res = set_actual_txgain(tmp->subs[SUB_REAL].dfd, channel, gain, tmp->law);
		else
			res = set_actual_rxgain(tmp->subs[SUB_REAL].dfd, channel, gain, tmp->law);

		if (res) {
			ast_cli(a->fd, "Unable to set the software gain for channel %d\n", channel);
			ast_mutex_unlock(lock);
			return CLI_FAILURE;
		}

		ast_cli(a->fd, "software %s gain set to %.1f on channel %d\n",
			tx ? "tx" : "rx", gain, channel);
		break;
	}
	ast_mutex_unlock(lock);

	if (tmp)
		return CLI_SUCCESS;

	ast_cli(a->fd, "Unable to find given channel %d\n", channel);
	return CLI_FAILURE;

}

static char *dahdi_set_dnd(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int channel;
	int on;
	struct dahdi_pvt *dahdi_chan = NULL;

	switch (cmd) {
	case CLI_INIT:
		e->command = "dahdi set dnd";
		e->usage =
			"Usage: dahdi set dnd <chan#> <on|off>\n"
			"   Sets/resets DND (Do Not Disturb) mode on a channel.\n"
			"   Changes take effect immediately.\n"
			"   <chan num> is the channel number\n"
			"   <on|off> Enable or disable DND mode?\n"
			;
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 5)
		return CLI_SHOWUSAGE;

	if ((channel = atoi(a->argv[3])) <= 0) {
		ast_cli(a->fd, "Expected channel number, got '%s'\n", a->argv[3]);
		return CLI_SHOWUSAGE;
	}

	if (ast_true(a->argv[4]))
		on = 1;
	else if (ast_false(a->argv[4]))
		on = 0;
	else {
		ast_cli(a->fd, "Expected 'on' or 'off', got '%s'\n", a->argv[4]);
		return CLI_SHOWUSAGE;
	}

	ast_mutex_lock(&iflock);
	for (dahdi_chan = iflist; dahdi_chan; dahdi_chan = dahdi_chan->next) {
		if (dahdi_chan->channel != channel)
			continue;

		/* Found the channel. Actually set it */
		dahdi_dnd(dahdi_chan, on);
		break;
	}
	ast_mutex_unlock(&iflock);

	if (!dahdi_chan) {
		ast_cli(a->fd, "Unable to find given channel %d\n", channel);
		return CLI_FAILURE;
	}

	return CLI_SUCCESS;
}

static struct ast_cli_entry dahdi_cli[] = {
	AST_CLI_DEFINE(handle_dahdi_show_cadences, "List cadences"),
	AST_CLI_DEFINE(dahdi_show_channels, "Show active DAHDI channels"),
	AST_CLI_DEFINE(dahdi_show_channel, "Show information on a channel"),
	AST_CLI_DEFINE(dahdi_destroy_channel, "Destroy a channel"),
	AST_CLI_DEFINE(dahdi_restart_cmd, "Fully restart DAHDI channels"),
	AST_CLI_DEFINE(dahdi_show_status, "Show all DAHDI cards status"),
	AST_CLI_DEFINE(dahdi_show_version, "Show the DAHDI version in use"),
	AST_CLI_DEFINE(dahdi_set_hwgain, "Set hardware gain on a channel"),
	AST_CLI_DEFINE(dahdi_set_swgain, "Set software gain on a channel"),
	AST_CLI_DEFINE(dahdi_set_dnd, "Set software gain on a channel"),
};

#define TRANSFER	0
#define HANGUP		1

static int dahdi_fake_event(struct dahdi_pvt *p, int mode)
{
	if (p) {
		switch (mode) {
			case TRANSFER:
				p->fake_event = DAHDI_EVENT_WINKFLASH;
				break;
			case HANGUP:
				p->fake_event = DAHDI_EVENT_ONHOOK;
				break;
			default:
				ast_log(LOG_WARNING, "I don't know how to handle transfer event with this: %d on channel %s\n",mode, p->owner->name);
		}
	}
	return 0;
}
static struct dahdi_pvt *find_channel(int channel)
{
	struct dahdi_pvt *p = iflist;
	while (p) {
		if (p->channel == channel) {
			break;
		}
		p = p->next;
	}
	return p;
}

static int action_dahdidndon(struct mansession *s, const struct message *m)
{
	struct dahdi_pvt *p = NULL;
	const char *channel = astman_get_header(m, "DAHDIChannel");

	if (ast_strlen_zero(channel)) {
		astman_send_error(s, m, "No channel specified");
		return 0;
	}
	p = find_channel(atoi(channel));
	if (!p) {
		astman_send_error(s, m, "No such channel");
		return 0;
	}
	p->dnd = 1;
	astman_send_ack(s, m, "DND Enabled");
	return 0;
}

static int action_dahdidndoff(struct mansession *s, const struct message *m)
{
	struct dahdi_pvt *p = NULL;
	const char *channel = astman_get_header(m, "DAHDIChannel");

	if (ast_strlen_zero(channel)) {
		astman_send_error(s, m, "No channel specified");
		return 0;
	}
	p = find_channel(atoi(channel));
	if (!p) {
		astman_send_error(s, m, "No such channel");
		return 0;
	}
	p->dnd = 0;
	astman_send_ack(s, m, "DND Disabled");
	return 0;
}

static int action_transfer(struct mansession *s, const struct message *m)
{
	struct dahdi_pvt *p = NULL;
	const char *channel = astman_get_header(m, "DAHDIChannel");

	if (ast_strlen_zero(channel)) {
		astman_send_error(s, m, "No channel specified");
		return 0;
	}
	p = find_channel(atoi(channel));
	if (!p) {
		astman_send_error(s, m, "No such channel");
		return 0;
	}
	dahdi_fake_event(p,TRANSFER);
	astman_send_ack(s, m, "DAHDITransfer");
	return 0;
}

static int action_transferhangup(struct mansession *s, const struct message *m)
{
	struct dahdi_pvt *p = NULL;
	const char *channel = astman_get_header(m, "DAHDIChannel");

	if (ast_strlen_zero(channel)) {
		astman_send_error(s, m, "No channel specified");
		return 0;
	}
	p = find_channel(atoi(channel));
	if (!p) {
		astman_send_error(s, m, "No such channel");
		return 0;
	}
	dahdi_fake_event(p,HANGUP);
	astman_send_ack(s, m, "DAHDIHangup");
	return 0;
}

static int action_dahdidialoffhook(struct mansession *s, const struct message *m)
{
	struct dahdi_pvt *p = NULL;
	const char *channel = astman_get_header(m, "DAHDIChannel");
	const char *number = astman_get_header(m, "Number");
	int i;

	if (ast_strlen_zero(channel)) {
		astman_send_error(s, m, "No channel specified");
		return 0;
	}
	if (ast_strlen_zero(number)) {
		astman_send_error(s, m, "No number specified");
		return 0;
	}
	p = find_channel(atoi(channel));
	if (!p) {
		astman_send_error(s, m, "No such channel");
		return 0;
	}
	if (!p->owner) {
		astman_send_error(s, m, "Channel does not have it's owner");
		return 0;
	}
	for (i = 0; i < strlen(number); i++) {
		struct ast_frame f = { AST_FRAME_DTMF, number[i] };
		dahdi_queue_frame(p, &f, NULL);
	}
	astman_send_ack(s, m, "DAHDIDialOffhook");
	return 0;
}

static int action_dahdishowchannels(struct mansession *s, const struct message *m)
{
	struct dahdi_pvt *tmp = NULL;
	const char *id = astman_get_header(m, "ActionID");
	const char *dahdichannel = astman_get_header(m, "DAHDIChannel");
	char idText[256] = "";
	int channels = 0;
	int dahdichanquery = -1;
	if (!ast_strlen_zero(dahdichannel)) {
		dahdichanquery = atoi(dahdichannel);
	}

	astman_send_ack(s, m, "DAHDI channel status will follow");
	if (!ast_strlen_zero(id))
		snprintf(idText, sizeof(idText), "ActionID: %s\r\n", id);

	ast_mutex_lock(&iflock);

	tmp = iflist;
	while (tmp) {
		if (tmp->channel > 0) {
			int alarm = get_alarms(tmp);

			/* If a specific channel is queried for, only deliver status for that channel */
			if (dahdichanquery > 0 && tmp->channel != dahdichanquery)
				continue;

			channels++;
			if (tmp->owner) {
				/* Add data if we have a current call */
				astman_append(s,
					"Event: DAHDIShowChannels\r\n"
					"DAHDIChannel: %d\r\n"
					"Channel: %s\r\n"
					"Uniqueid: %s\r\n"
					"AccountCode: %s\r\n"
					"Signalling: %s\r\n"
					"SignallingCode: %d\r\n"
					"Context: %s\r\n"
					"DND: %s\r\n"
					"Alarm: %s\r\n"
					"%s"
					"\r\n",
					tmp->channel,
					tmp->owner->name,
					tmp->owner->uniqueid,
					tmp->owner->accountcode,
					sig2str(tmp->sig),
					tmp->sig,
					tmp->context,
					tmp->dnd ? "Enabled" : "Disabled",
					alarm2str(alarm), idText);
			} else {
				astman_append(s,
					"Event: DAHDIShowChannels\r\n"
					"DAHDIChannel: %d\r\n"
					"Signalling: %s\r\n"
					"SignallingCode: %d\r\n"
					"Context: %s\r\n"
					"DND: %s\r\n"
					"Alarm: %s\r\n"
					"%s"
					"\r\n",
					tmp->channel, sig2str(tmp->sig), tmp->sig,
					tmp->context,
					tmp->dnd ? "Enabled" : "Disabled",
					alarm2str(alarm), idText);
			}
		}

		tmp = tmp->next;
	}

	ast_mutex_unlock(&iflock);

	astman_append(s,
		"Event: DAHDIShowChannelsComplete\r\n"
		"%s"
		"Items: %d\r\n"
		"\r\n",
		idText,
		channels);
	return 0;
}

#ifdef HAVE_SS7
static int linkset_addsigchan(int sigchan)
{
	struct dahdi_ss7 *link;
	int res;
	int curfd;
	struct dahdi_params p;
	struct dahdi_bufferinfo bi;
	struct dahdi_spaninfo si;


	link = ss7_resolve_linkset(cur_linkset);
	if (!link) {
		ast_log(LOG_ERROR, "Invalid linkset number.  Must be between 1 and %d\n", NUM_SPANS + 1);
		return -1;
	}

	if (cur_ss7type < 0) {
		ast_log(LOG_ERROR, "Unspecified or invalid ss7type\n");
		return -1;
	}

	if (!link->ss7)
		link->ss7 = ss7_new(cur_ss7type);

	if (!link->ss7) {
		ast_log(LOG_ERROR, "Can't create new SS7!\n");
		return -1;
	}

	link->type = cur_ss7type;

	if (cur_pointcode < 0) {
		ast_log(LOG_ERROR, "Unspecified pointcode!\n");
		return -1;
	} else
		ss7_set_pc(link->ss7, cur_pointcode);

	if (sigchan < 0) {
		ast_log(LOG_ERROR, "Invalid sigchan!\n");
		return -1;
	} else {
		if (link->numsigchans >= NUM_DCHANS) {
			ast_log(LOG_ERROR, "Too many sigchans on linkset %d\n", cur_linkset);
			return -1;
		}
		curfd = link->numsigchans;

		link->fds[curfd] = open("/dev/dahdi/channel", O_RDWR, 0600);
		if ((link->fds[curfd] < 0) || (ioctl(link->fds[curfd],DAHDI_SPECIFY,&sigchan) == -1)) {
			ast_log(LOG_ERROR, "Unable to open SS7 sigchan %d (%s)\n", sigchan, strerror(errno));
			return -1;
		}
		memset(&p, 0, sizeof(p));
		res = ioctl(link->fds[curfd], DAHDI_GET_PARAMS, &p);
		if (res) {
			dahdi_close(link->fds[curfd]);
			link->fds[curfd] = -1;
			ast_log(LOG_ERROR, "Unable to get parameters for sigchan %d (%s)\n", sigchan, strerror(errno));
			return -1;
		}
		if ((p.sigtype != DAHDI_SIG_HDLCFCS) && (p.sigtype != DAHDI_SIG_HARDHDLC) && (p.sigtype != DAHDI_SIG_MTP2)) {
			dahdi_close(link->fds[curfd]);
			link->fds[curfd] = -1;
			ast_log(LOG_ERROR, "sigchan %d is not in HDLC/FCS mode.\n", sigchan);
			return -1;
		}

		memset(&bi, 0, sizeof(bi));
		bi.txbufpolicy = DAHDI_POLICY_IMMEDIATE;
		bi.rxbufpolicy = DAHDI_POLICY_IMMEDIATE;
		bi.numbufs = 32;
		bi.bufsize = 512;

		if (ioctl(link->fds[curfd], DAHDI_SET_BUFINFO, &bi)) {
			ast_log(LOG_ERROR, "Unable to set appropriate buffering on channel %d: %s\n", sigchan, strerror(errno));
			dahdi_close(link->fds[curfd]);
			link->fds[curfd] = -1;
			return -1;
		}

		if (p.sigtype == DAHDI_SIG_MTP2)
			ss7_add_link(link->ss7, SS7_TRANSPORT_DAHDIMTP2, link->fds[curfd]);
		else
			ss7_add_link(link->ss7, SS7_TRANSPORT_DAHDIDCHAN, link->fds[curfd]);

		link->numsigchans++;

		memset(&si, 0, sizeof(si));
		res = ioctl(link->fds[curfd], DAHDI_SPANSTAT, &si);
		if (res) {
			dahdi_close(link->fds[curfd]);
			link->fds[curfd] = -1;
			ast_log(LOG_ERROR, "Unable to get span state for sigchan %d (%s)\n", sigchan, strerror(errno));
		}

		if (!si.alarms) {
			link->linkstate[curfd] = LINKSTATE_DOWN;
			ss7_link_noalarm(link->ss7, link->fds[curfd]);
		} else {
			link->linkstate[curfd] = LINKSTATE_DOWN | LINKSTATE_INALARM;
			ss7_link_alarm(link->ss7, link->fds[curfd]);
		}
	}

	if (cur_adjpointcode < 0) {
		ast_log(LOG_ERROR, "Unspecified adjpointcode!\n");
		return -1;
	} else {
		ss7_set_adjpc(link->ss7, link->fds[curfd], cur_adjpointcode);
	}

	if (cur_defaultdpc < 0) {
		ast_log(LOG_ERROR, "Unspecified defaultdpc!\n");
		return -1;
	}

	if (cur_networkindicator < 0) {
		ast_log(LOG_ERROR, "Invalid networkindicator!\n");
		return -1;
	} else
		ss7_set_network_ind(link->ss7, cur_networkindicator);

	return 0;
}

static int ss7_find_alloc_call(struct dahdi_pvt *p) {
	if(!p)
		return 0;
	if(!p->ss7call) {
		p->ss7call = isup_new_call(p->ss7->ss7);
		if(!p->ss7call)
			return 0;
		else
			isup_init_call(p->ss7->ss7, p->ss7call, p->cic, p->dpc);
	}
	return 1;
}

static char *handle_ss7_no_debug(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int span;
	switch (cmd) {
	case CLI_INIT:
		e->command = "ss7 no debug linkset";
		e->usage =
			"Usage: ss7 no debug linkset <span>\n"
			"       Disables debugging on a given SS7 linkset\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (a->argc < 5)
		return CLI_SHOWUSAGE;
	span = atoi(a->argv[4]);
	if ((span < 1) || (span > NUM_SPANS)) {
		ast_cli(a->fd, "Invalid linkset %s.  Should be a number from %d to %d\n", a->argv[4], 1, NUM_SPANS);
		return CLI_SUCCESS;
	}
	if (!linksets[span-1].ss7) {
		ast_cli(a->fd, "No SS7 running on linkset %d\n", span);
		return CLI_SUCCESS;
	}
	if (linksets[span-1].ss7)
		ss7_set_debug(linksets[span-1].ss7, 0);

	ast_cli(a->fd, "Disabled debugging on linkset %d\n", span);
	return CLI_SUCCESS;
}

static char *handle_ss7_debug(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int span;
	switch (cmd) {
	case CLI_INIT:
		e->command = "ss7 debug linkset";
		e->usage =
			"Usage: ss7 debug linkset <linkset>\n"
			"       Enables debugging on a given SS7 linkset\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (a->argc < 4)
		return CLI_SHOWUSAGE;
	span = atoi(a->argv[3]);
	if ((span < 1) || (span > NUM_SPANS)) {
		ast_cli(a->fd, "Invalid linkset %s.  Should be a number from %d to %d\n", a->argv[3], 1, NUM_SPANS);
		return CLI_SUCCESS;
	}
	if (!linksets[span-1].ss7) {
		ast_cli(a->fd, "No SS7 running on linkset %d\n", span);
		return CLI_SUCCESS;
	}
	if (linksets[span-1].ss7)
		ss7_set_debug(linksets[span-1].ss7, SS7_DEBUG_MTP2 | SS7_DEBUG_MTP3 | SS7_DEBUG_ISUP);

	ast_cli(a->fd, "Enabled debugging on linkset %d\n", span);
	return CLI_SUCCESS;
}

static char *handle_ss7_block_cic(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int linkset, cic;
	int blocked = -1, i;
	unsigned int dpc;
	struct dahdi_pvt *p;

	switch (cmd) {
	case CLI_INIT:
		e->command = "ss7 block cic";
		e->usage =
			"Usage: ss7 block cic <linkset> <dpc> <CIC>\n"
			"       Sends a remote blocking request for the given CIC on the specified linkset\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (a->argc == 6)
		linkset = atoi(a->argv[3]);
	else
		return CLI_SHOWUSAGE;

	if ((linkset < 1) || (linkset > NUM_SPANS)) {
		ast_cli(a->fd, "Invalid linkset %s.  Should be a number %d to %d\n", a->argv[3], 1, NUM_SPANS);
		return CLI_SUCCESS;
	}

	if (!linksets[linkset-1].ss7) {
		ast_cli(a->fd, "No SS7 running on linkset %d\n", linkset);
		return CLI_SUCCESS;
	}

	cic = atoi(a->argv[5]);

	if (cic < 1) {
		ast_cli(a->fd, "Invalid CIC specified!\n");
		return CLI_SUCCESS;
	}

	dpc = atoi(a->argv[4]);

	for (i = 0; i < linksets[linkset-1].numchans; i++) {
		p = linksets[linkset-1].pvts[i];
		if (p->cic == cic && p->dpc == dpc) {
			blocked = p->locallyblocked;
			if (!(blocked & SS7_BLOCKED_MAINTENANCE)) {
				ast_mutex_lock(&p->lock);
				ss7_grab(p, p->ss7);
				if(!ss7_find_alloc_call(p)) {
					ss7_rel(p->ss7);
					ast_mutex_unlock(&p->lock);
					ast_cli(a->fd, "Unable allocate new ss7call\n");
					return CLI_SUCCESS;
				}
				isup_blo(linksets[linkset-1].ss7, p->ss7call);
				ss7_rel(p->ss7);
				ast_mutex_unlock(&p->lock);
			}
		}
	}

	if (blocked < 0) {
		ast_cli(a->fd, "Invalid CIC specified!\n");
		return CLI_SUCCESS;
	}

	if (!blocked & SS7_BLOCKED_MAINTENANCE)
		ast_cli(a->fd, "Sent blocking request for linkset %d on CIC %d\n", linkset, cic);
	else
		ast_cli(a->fd, "CIC %d already locally blocked\n", cic);

	/* Break poll on the linkset so it sends our messages */
	pthread_kill(linksets[linkset-1].master, SIGURG);

	return CLI_SUCCESS;
}

static char *handle_ss7_show_calls(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int linkset;

	switch (cmd) {
	case CLI_INIT:
		e->command = "ss7 show calls";
		e->usage =
			"Usage: ss7 show calls <linkset>\n"
			"       Show SS7 calls on the specified linkset\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (a->argc == 4)
		linkset = atoi(a->argv[3]);
	else
		return CLI_SHOWUSAGE;

	if ((linkset < 1) || (linkset > NUM_SPANS)) {
		ast_cli(a->fd, "Invalid linkset %s.  Should be a number %d to %d\n", a->argv[3], 1, NUM_SPANS);
		return CLI_SUCCESS;
	}

	if (!linksets[linkset-1].ss7) {
		ast_cli(a->fd, "No SS7 running on linkset %d\n", linkset);
		return CLI_SUCCESS;
	}

	ast_mutex_lock(&linksets[linkset-1].lock);
	isup_show_calls(linksets[linkset-1].ss7, &ast_cli, a->fd);
	ast_mutex_unlock(&linksets[linkset-1].lock);

	return CLI_SUCCESS;
}

static char *handle_ss7_reset_cic(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int linkset, cic;
	int i, res;
	unsigned int dpc;
	struct dahdi_pvt *p;

	switch (cmd) {
	case CLI_INIT:
		e->command = "ss7 reset cic";
		e->usage =
			"Usage: ss7 reset cic <linkset> <dpc> <CIC>\n"
			"       Send a RSC for the given CIC on the specified linkset\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (a->argc == 6)
		linkset = atoi(a->argv[3]);
	else
		return CLI_SHOWUSAGE;

	if ((linkset < 1) || (linkset > NUM_SPANS)) {
		ast_cli(a->fd, "Invalid linkset %s.  Should be a number %d to %d\n", a->argv[3], 1, NUM_SPANS);
		return CLI_SUCCESS;
	}

	if (!linksets[linkset-1].ss7) {
		ast_cli(a->fd, "No SS7 running on linkset %d\n", linkset);
		return CLI_SUCCESS;
	}

	cic = atoi(a->argv[5]);

	if (cic < 1) {
		ast_cli(a->fd, "Invalid CIC specified!\n");
		return CLI_SUCCESS;
	}

	dpc = atoi(a->argv[4]);

	for (i = 0; i < linksets[linkset-1].numchans; i++) {
		p = linksets[linkset-1].pvts[i];
		if (p->cic == cic && p->dpc == dpc) {
			ast_mutex_lock(&p->lock);
			p->locallyblocked = 0;
			ss7_grab(p, p->ss7);
			res = ss7_start_rsc(p);
			ss7_rel(p->ss7);
			ast_mutex_unlock(&p->lock);
			ast_cli(a->fd, "%s RSC for linkset %d on CIC %d DPC %d\n", res ? "Sent" : "Failed", linkset, cic, dpc);
		}
	}

	return CLI_SUCCESS;
}

static char *handle_ss7_group_reset(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int linkset, cic, range;
	int i;
	unsigned int dpc;

	struct dahdi_pvt *p;
	switch (cmd) {
	case CLI_INIT:
		e->command = "ss7 reset group";
		e->usage =
			"Usage: ss7 reset group <linkset> <dpc> <1st CIC> <range>\n"
			"       Send a GRS for the given CIC range on the specified linkset\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (a->argc == 7)
		linkset = atoi(a->argv[3]);
	else
		return CLI_SHOWUSAGE;

	if ((linkset < 1) || (linkset > NUM_SPANS)) {
		ast_cli(a->fd, "Invalid linkset %s.  Should be a number %d to %d\n", a->argv[4], 1, NUM_SPANS);
		return CLI_SUCCESS;
	}

	if (!linksets[linkset-1].ss7) {
		ast_cli(a->fd, "No SS7 running on linkset %d\n", linkset);
		return CLI_SUCCESS;
	}

	cic = atoi(a->argv[5]);

	if (cic < 1) {
		ast_cli(a->fd, "Invalid CIC specified!\n");
		return CLI_SUCCESS;
	}

	range = atoi(a->argv[6]);
	if (range < 1 || range > 31) {
		ast_cli(a->fd, "Invalid range specified!\n");
		return CLI_SUCCESS;
	}

	dpc = atoi(a->argv[4]);

	if(!ss7_find_cic_range(&linksets[linkset-1], cic, cic + range, dpc)) {
		ast_cli(a->fd, "Invalid CIC/RANGE\n");
		return CLI_SHOWUSAGE;
	}

	for (i = 0; i < linksets[linkset-1].numchans; i++) {
		p = linksets[linkset-1].pvts[i];
		if (p->cic == cic && p->dpc == dpc) {
			ast_mutex_lock(&p->lock);
			ss7_grab(p, p->ss7);
			if(!ss7_find_alloc_call(p)) {
				ss7_rel(p->ss7);
				ast_mutex_unlock(&p->lock);
				ast_cli(a->fd, "Unable allocate new ss7call\n");
				return CLI_SUCCESS;
			}
			ss7_clear_channels(p, p->cic + range, SS7_HANGUP_FREE_CALL);
			ss7_block_cics(&linksets[linkset-1], p->cic, p->cic + range, dpc, NULL, 0, 1,
				SS7_BLOCKED_MAINTENANCE | SS7_BLOCKED_HARDWARE);
			ss7_block_cics(&linksets[linkset-1], p->cic, p->cic + range, dpc, NULL, 0, 0,
				SS7_BLOCKED_MAINTENANCE | SS7_BLOCKED_HARDWARE);
			isup_grs(linksets[linkset-1].ss7, p->ss7call, p->cic + range);
			ss7_rel(p->ss7);
			ast_mutex_unlock(&p->lock);
			ast_cli(a->fd, "GRS sent ... \n");
		}
	}

	return CLI_SUCCESS;
}

static char *handle_ss7_group_block(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int linkset, cic, cic_offset, range, chanpos = -1;
	int i, dpc, orient = 0;
	unsigned char state[255];
	struct dahdi_pvt *p;

	switch (cmd) {
	case CLI_INIT:
		e->command = "ss7 block group";
		e->usage =
			"Usage: ss7 block group <linkset> <dpc> <1st. CIC> <range> [H]\n"
			"       Sends a remote blocking request for CIC range on the specified linkset\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc == 7 || a->argc == 8)
		linkset = atoi(a->argv[3]);
	else
		return CLI_SHOWUSAGE;

	if(a->argc == 8) {
		if(a->argv[7][0] == 'H')
			orient = 1;
		else
			return CLI_SHOWUSAGE;
	}

	if ((linkset < 1) || (linkset > NUM_SPANS)) {
		ast_cli(a->fd, "Invalid linkset %s.  Should be a number %d to %d\n", a->argv[4], 1, NUM_SPANS);
		return CLI_SUCCESS;
	}

	if (!linksets[linkset-1].ss7) {
		ast_cli(a->fd, "No SS7 running on linkset %d\n", linkset);
		return CLI_SUCCESS;
	}

	cic = atoi(a->argv[5]);

	if (cic < 1) {
		ast_cli(a->fd, "Invalid CIC specified!\n");
		return CLI_SUCCESS;
	}

	range = atoi(a->argv[6]);
	if (range < 1 || range > 31) {
		ast_cli(a->fd, "Invalid range specified!\n");
		return CLI_SUCCESS;
	}

	dpc = atoi(a->argv[4]);

	if(!ss7_find_cic_range(&linksets[linkset-1], cic, cic + range, dpc)) {
		ast_cli(a->fd, "Invalid CIC/RANGE\n");
		return CLI_SHOWUSAGE;
	}

	cic_offset = 0;
	memset(state, 0, sizeof(state));
	for (i = 0; i < linksets[linkset-1].numchans; i++) {
		p = linksets[linkset-1].pvts[i];
		if (p->cic >= cic && p->cic <= cic + range && p->dpc == dpc) {
			state[cic_offset] = 1;
			cic_offset ++;
		}
		if (p->cic == cic && p->dpc == dpc) {
			chanpos = i;
		}
	}

	if (chanpos >= 0) {
		p = linksets[linkset-1].pvts[chanpos];
		ast_mutex_lock(&p->lock);
		ss7_grab(p, p->ss7);
		if(!ss7_find_alloc_call(p)) {
			ss7_rel(p->ss7);
			ast_mutex_unlock(&p->lock);
			ast_cli(a->fd, "Unable allocate new ss7call\n");
			return CLI_SUCCESS;
		}

		isup_cgb(linksets[linkset-1].ss7, p->ss7call, cic + range, state, orient);
		ss7_rel(p->ss7);
		ast_mutex_unlock(&p->lock);
		ast_cli(a->fd, "Sending remote blocking request linkset %d on CIC %d range %d\n", linkset, cic, range);

	}

	return CLI_SUCCESS;
}

static char *handle_ss7_group_unblock(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int linkset, cic, cic_offset, range, chanpos = -1;
	int i, dpc, orient = 0;
	unsigned char state[255];
	struct dahdi_pvt *p;

	switch (cmd) {
	case CLI_INIT:
		e->command = "ss7 unblock group";
		e->usage =
			"Usage: ss7 unblock group <linkset> <dpc> <1st. CIC> <range> [H]\n"
			"       Sends a remote unblocking request for CIC range on the specified linkset\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc == 7 || a->argc == 8)
		linkset = atoi(a->argv[3]);
	else
		return CLI_SHOWUSAGE;

	if(a->argc == 8) {
		if(a->argv[7][0] == 'H')
			orient = 1;
		else
			return CLI_SHOWUSAGE;
	}

	if ((linkset < 1) || (linkset > NUM_SPANS)) {
		ast_cli(a->fd, "Invalid linkset %s.  Should be a number %d to %d\n", a->argv[4], 1, NUM_SPANS);
		return CLI_SUCCESS;
	}

	if (!linksets[linkset-1].ss7) {
		ast_cli(a->fd, "No SS7 running on linkset %d\n", linkset);
		return CLI_SUCCESS;
	}

	cic = atoi(a->argv[5]);

	if (cic < 1) {
		ast_cli(a->fd, "Invalid CIC specified!\n");
		return CLI_SUCCESS;
	}

	range = atoi(a->argv[6]);
	if (range < 1 || range > 31) {
		ast_cli(a->fd, "Invalid range specified!\n");
		return CLI_SUCCESS;
	}

	dpc = atoi(a->argv[4]);

	if(!ss7_find_cic_range(&linksets[linkset-1], cic, cic + range, dpc)) {
		ast_cli(a->fd, "Invalid CIC/RANGE\n");
		return CLI_SHOWUSAGE;
	}

	cic_offset = 0;
	memset(state, 0, sizeof(state));
	for (i = 0; i < linksets[linkset-1].numchans; i++) {
		p = linksets[linkset-1].pvts[i];
		if (p->cic >= cic && p->cic <= cic + range && p->dpc == dpc) {
			state[cic_offset] = 1;
			cic_offset ++;
		}
		if (p->cic == cic && p->dpc == dpc) {
			chanpos = i;
		}
	}

	if (chanpos >= 0) {
		p = linksets[linkset-1].pvts[chanpos];
		ast_mutex_lock(&p->lock);
		ss7_grab(p, p->ss7);
		if(!ss7_find_alloc_call(p)) {
			ss7_rel(p->ss7);
			ast_mutex_unlock(&p->lock);
			ast_cli(a->fd, "Unable allocate new ss7call\n");
			return CLI_SUCCESS;
		}

		isup_cgu(linksets[linkset-1].ss7, p->ss7call, cic + range, state, orient);
		ss7_rel(p->ss7);
		ast_mutex_unlock(&p->lock);
		ast_cli(a->fd, "Sending remote unblocking request linkset %d on CIC %d range %d\n", linkset, cic, range);

	}

	return CLI_SUCCESS;
}


static char *handle_ss7_block_linkset(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int linkset;
	int i;
	struct dahdi_pvt *p;

	switch (cmd) {
	case CLI_INIT:
		e->command = "ss7 block linkset";
		e->usage =
			"Usage: ss7 block linkset <linkset number>\n"
			"       Sends a remote blocking request for all CICs on the given linkset\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (a->argc == 4)
		linkset = atoi(a->argv[3]);
	else
		return CLI_SHOWUSAGE;

	if ((linkset < 1) || (linkset > NUM_SPANS)) {
		ast_cli(a->fd, "Invalid linkset %s.  Should be a number %d to %d\n", a->argv[3], 1, NUM_SPANS);
		return CLI_SUCCESS;
	}

	if (!linksets[linkset-1].ss7) {
		ast_cli(a->fd, "No SS7 running on linkset %d\n", linkset);
		return CLI_SUCCESS;
	}

	for (i = 0; i < linksets[linkset-1].numchans; i++) {
		p = linksets[linkset-1].pvts[i];
		ast_mutex_lock(&p->lock);
		ss7_grab(p, p->ss7);
		if(ss7_find_alloc_call(p)) {
			isup_blo(linksets[linkset-1].ss7, p->ss7call);
			ss7_rel(p->ss7);
			ast_cli(a->fd, "Sending remote blocking request on CIC %d DPC %d\n", p->cic, p->dpc);
		} else {
			ss7_rel(p->ss7);
		}
		ast_mutex_unlock(&p->lock);
	}

	return CLI_SUCCESS;
}

static char *handle_ss7_unblock_cic(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int linkset, cic;
	int i, blocked = -1;
	unsigned int dpc;
	struct dahdi_pvt *p;

	switch (cmd) {
	case CLI_INIT:
		e->command = "ss7 unblock cic";
		e->usage =
			"Usage: ss7 unblock cic <linkset> <dpc> <CIC>\n"
			"       Sends a remote unblocking request for the given CIC on the specified linkset\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc == 6)
		linkset = atoi(a->argv[3]);
	else
		return CLI_SHOWUSAGE;

	if ((linkset < 1) || (linkset > NUM_SPANS)) {
		ast_cli(a->fd, "Invalid linkset %s.  Should be a number %d to %d\n", a->argv[3], 1, NUM_SPANS);
		return CLI_SUCCESS;
	}

	if (!linksets[linkset-1].ss7) {
		ast_cli(a->fd, "No SS7 running on linkset %d\n", linkset);
		return CLI_SUCCESS;
	}

	cic = atoi(a->argv[5]);

	if (cic < 1) {
		ast_cli(a->fd, "Invalid CIC specified!\n");
		return CLI_SUCCESS;
	}

	dpc = atoi(a->argv[4]);

	for (i = 0; i < linksets[linkset-1].numchans; i++) {
		p = linksets[linkset-1].pvts[i];
		if (p->cic == cic && p->dpc == dpc) {
			blocked = p->locallyblocked;
			if (blocked) {
				ast_mutex_lock(&p->lock);
				ss7_grab(p, p->ss7);
				if(!ss7_find_alloc_call(p)) {
					ss7_rel(p->ss7);
					ast_mutex_unlock(&p->lock);
					ast_cli(a->fd, "Unable allocate new ss7call\n");
					return CLI_SUCCESS;
				}
				isup_ubl(linksets[linkset-1].ss7, p->ss7call);
				ss7_rel(p->ss7);
				ast_mutex_unlock(&p->lock);
			}
		}
	}

	if (blocked > 0)
		ast_cli(a->fd, "Sent unblocking request for linkset %d on CIC %d\n", linkset, cic);

	return CLI_SUCCESS;
}

static char *handle_ss7_unblock_linkset(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int linkset;
	int i;
	struct dahdi_pvt *p;

	switch (cmd) {
	case CLI_INIT:
		e->command = "ss7 unblock linkset";
		e->usage =
			"Usage: ss7 unblock linkset <linkset number>\n"
			"       Sends a remote unblocking request for all CICs on the specified linkset\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc == 4)
		linkset = atoi(a->argv[3]);
	else
		return CLI_SHOWUSAGE;

	if ((linkset < 1) || (linkset > NUM_SPANS)) {
		ast_cli(a->fd, "Invalid linkset %s.  Should be a number %d to %d\n", a->argv[3], 1, NUM_SPANS);
		return CLI_SUCCESS;
	}

	if (!linksets[linkset-1].ss7) {
		ast_cli(a->fd, "No SS7 running on linkset %d\n", linkset);
		return CLI_SUCCESS;
	}

	for (i = 0; i < linksets[linkset-1].numchans; i++) {
		p = linksets[linkset-1].pvts[i];
		ast_mutex_lock(&p->lock);
		ss7_grab(p, p->ss7);
		if(ss7_find_alloc_call(p)) {
			isup_ubl(linksets[linkset-1].ss7, p->ss7call);
			ast_cli(a->fd, "Sending remote blocking request on CIC %d DPC %d\n", p->cic, p->dpc);
		}
		ss7_rel(p->ss7);
		ast_mutex_unlock(&p->lock);
	}

	return CLI_SUCCESS;
}

static char *handle_ss7_show_cics(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int i, linkset, dpc = 0;
	struct dahdi_ss7 *ss7;
	static char format[] = "%5s %5s %6s %12s   %-12s\n";
	static char formatd[] = "%5i %5i %6i %12s   %-12s\n";
	char *state;
	char blocking[12];

	switch (cmd) {
	case CLI_INIT:
		e->command = "ss7 show cics";
		e->usage =
			"Usage: ss7 show cics <linkset> [dpc]\n"
			"       Shows the cics of an SS7 linkset.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc < 4 || a->argc > 5)
		return CLI_SHOWUSAGE;
	linkset = atoi(a->argv[3]);

	if ((linkset < 1) || (linkset > NUM_SPANS)) {
		ast_cli(a->fd, "Invalid linkset %s.  Should be a number %d to %d\n", a->argv[3], 1, NUM_SPANS);
		return CLI_SUCCESS;
	}

	if (!linksets[linkset-1].ss7) {
		ast_cli(a->fd, "No SS7 running on linkset %d\n", linkset);
		return CLI_SUCCESS;
	}

	if (linksets[linkset-1].ss7)
		ss7 = &linksets[linkset-1];

	if(a->argc == 5)
		dpc = atoi(a->argv[4]);

	ast_cli(a->fd, format, "CIC", "DPC", "DAHDI", "STATE", "BLOCKING");

	for (i = 0; i < ss7->numchans; i++) {
		if(!dpc || ss7->pvts[i]->dpc == dpc) {

			if(ss7->pvts[i]->owner)
				state = "Used";
			else if (ss7->pvts[i]->ss7call)
				state = "Pending";
			else if (!ss7->pvts[i]->inservice)
				state = "NotInServ";
			else
				state = "Idle";

			if (ss7->pvts[i]->locallyblocked) {
				strcpy(blocking, "L:");
				if(ss7->pvts[i]->locallyblocked & SS7_BLOCKED_MAINTENANCE)
					strcat(blocking, "M");
				else
					strcat(blocking, " ");

				if(ss7->pvts[i]->locallyblocked & SS7_BLOCKED_HARDWARE)
					strcat(blocking, "H");
				else
					strcat(blocking, " ");

			} else {
				strcpy(blocking, "    ");
			}

			if (ss7->pvts[i]->remotelyblocked) {
				strcat(blocking, " R:");
				if(ss7->pvts[i]->remotelyblocked & SS7_BLOCKED_MAINTENANCE)
					strcat(blocking, "M");
				else
					strcat(blocking, " ");

				if(ss7->pvts[i]->remotelyblocked & SS7_BLOCKED_HARDWARE)
					strcat(blocking, "H");
				else
					strcat(blocking, " ");

			}


			ast_cli(a->fd, formatd, ss7->pvts[i]->cic, ss7->pvts[i]->dpc, ss7->pvts[i]->channel, state, blocking);
		}
	}

	return CLI_SUCCESS;
}

static char *handle_ss7_show_linkset(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int linkset;
	struct dahdi_ss7 *ss7;
	switch (cmd) {
	case CLI_INIT:
		e->command = "ss7 show linkset";
		e->usage =
			"Usage: ss7 show linkset <span>\n"
			"       Shows the status of an SS7 linkset.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc < 4)
		return CLI_SHOWUSAGE;
	linkset = atoi(a->argv[3]);
	if ((linkset < 1) || (linkset > NUM_SPANS)) {
		ast_cli(a->fd, "Invalid linkset %s.  Should be a number %d to %d\n", a->argv[3], 1, NUM_SPANS);
		return CLI_SUCCESS;
	}
	if (!linksets[linkset-1].ss7) {
		ast_cli(a->fd, "No SS7 running on linkset %d\n", linkset);
		return CLI_SUCCESS;
	}
	if (linksets[linkset-1].ss7)
		ss7 = &linksets[linkset-1];

	ast_cli(a->fd, "SS7 flags: 0x%x\n", ss7->flags);
	ast_cli(a->fd, "SS7 linkset %d status: %s\n", linkset, (ss7->state == LINKSET_STATE_UP) ? "Up" : "Down");
	ast_cli(a->fd, "SS7 calling nai: %i\n", ss7->calling_nai);
	ast_cli(a->fd, "SS7 called nai: %i\n", ss7->called_nai);
	ast_cli(a->fd, "SS7 nationalprefix: %s\n", ss7->nationalprefix);
	ast_cli(a->fd, "SS7 internationalprefix: %s\n", ss7->internationalprefix);
	ast_cli(a->fd, "SS7 unknownprefix: %s\n", ss7->unknownprefix);
	ast_cli(a->fd, "SS7 networkroutedprefix: %s\n", ss7->networkroutedprefix);
	ast_cli(a->fd, "SS7 subscriberprefix: %s\n", ss7->subscriberprefix);
	ss7_show_linkset(ss7->ss7, &ast_cli, a->fd);
	return CLI_SUCCESS;
}

static char *handle_ss7_mtp3_restart(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int linkset;
	struct dahdi_ss7 *ss7;
	unsigned int slc = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "ss7 restart mtp3";
		e->usage =
			"Usage: ss7 restart mtp3 <linkset> <slc>\n"
			"       Restart link\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc < 5)
		return CLI_SHOWUSAGE;
	linkset = atoi(a->argv[3]);
	if ((linkset < 1) || (linkset > NUM_SPANS)) {
		ast_cli(a->fd, "Invalid linkset %s.  Should be a number %d to %d\n", a->argv[2], 1, NUM_SPANS);
		return CLI_SUCCESS;
	}
	if (!linksets[linkset-1].ss7) {
		ast_cli(a->fd, "No SS7 running on linkset %d\n", linkset);
		return CLI_SUCCESS;
	}
	if (linksets[linkset-1].ss7)
		ss7 = &linksets[linkset-1];

	slc = atoi(a->argv[4]);

	ast_mutex_lock(&ss7->lock);
	mtp3_init_restart(ss7->ss7, slc);
	pthread_kill(ss7->master, SIGURG);
	ast_mutex_unlock(&ss7->lock);

	return CLI_SUCCESS;
}

static char *handle_ss7_net_mnt(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int linkset;
	struct dahdi_ss7 *ss7;
	unsigned int slc = 0;
	unsigned int arg = 0;
	char *res;

	switch (cmd) {
	case CLI_INIT:
		e->command = "ss7 mtp3";
		e->usage =
			"Usage: ss7 mtp3 <linkset> <slc> coo|coa|cbd|cba|eco|eca|tfp|tfa|lin|lun|lia|lua|lid|lfu <arg>\n"
			"       Send and NET MNT message\n"
			"       WARNING!!! WARNING!!! we are not an stp, just for testing/development purposes\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc < 5)
		return CLI_SHOWUSAGE;
	linkset = atoi(a->argv[2]);
	if ((linkset < 1) || (linkset > NUM_SPANS)) {
		ast_cli(a->fd, "Invalid linkset %s.  Should be a number %d to %d\n", a->argv[2], 1, NUM_SPANS);
		return CLI_SUCCESS;
	}
	if (!linksets[linkset-1].ss7) {
		ast_cli(a->fd, "No SS7 running on linkset %d\n", linkset);
		return CLI_SUCCESS;
	}
	if (linksets[linkset-1].ss7)
		ss7 = &linksets[linkset-1];

	slc = atoi(a->argv[3]);

	if (a->argc == 6)
		arg = atoi(a->argv[5]);

	ast_mutex_lock(&ss7->lock);
	res = mtp3_net_mng(ss7->ss7, slc, a->argv[4], arg);
	pthread_kill(ss7->master, SIGURG);
	ast_mutex_unlock(&ss7->lock);

	ast_cli(a->fd, "%s", res);

	return CLI_SUCCESS;
}

static char *handle_ss7_destroy_cics(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int linkset, cicbegin, cicend, cur_cic, cur_chan, channel, res;
	unsigned short dpc;
	struct dahdi_ss7 *ss7;

	switch (cmd) {
	case CLI_INIT:
		e->command = "ss7 destroy cics";
		e->usage =
			"Usage: ss7 destroy cics <linkset> <cicbegin> <cicend> <dpc>\n"
			"       Destroy cics\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc < 7)
		return CLI_SHOWUSAGE;
	linkset = atoi(a->argv[3]);

	if ((linkset < 1) || (linkset > NUM_SPANS)) {
		ast_cli(a->fd, "Invalid linkset %s.  Should be a number %d to %d\n", a->argv[2], 1, NUM_SPANS);
		return CLI_SUCCESS;
	}
	if (!linksets[linkset-1].ss7) {
		ast_cli(a->fd, "No SS7 running on linkset %d\n", linkset);
		return CLI_SUCCESS;
	}

	if (linksets[linkset-1].ss7)
		ss7 = &linksets[linkset-1];

	cicbegin = atoi(a->argv[4]);
	cicend = atoi(a->argv[5]);
	dpc = atoi(a->argv[6]);

	if(!cicbegin || !cicend || !dpc || cicend < cicbegin) {
		ast_cli(a->fd, "Invalid cicbegin/cicend/dpc\n");
		return CLI_SUCCESS;
	}

	for (cur_cic = cicbegin; cur_cic <= cicend; cur_cic++) {
		if (ss7_find_cic(ss7, cur_cic, dpc) == -1) {
			ast_cli(a->fd, "CIC: %i DPC: %i doesn't exist\n", cur_cic, dpc);
			return CLI_SUCCESS;
		}
	}

	ast_mutex_lock(&ss7->lock);
	for (cur_cic = cicbegin; cur_cic <= cicend; cur_cic++) {
		channel = ss7_find_cic(ss7, cur_cic, dpc);
		ast_mutex_lock(&ss7->pvts[channel]->lock);
		if(ss7->pvts[channel]->ss7call) {
			isup_free_call(ss7->ss7, ss7->pvts[channel]->ss7call);
			ss7->pvts[channel]->ss7call = NULL;
		}
		ast_mutex_unlock(&ss7->pvts[channel]->lock);
		res = dahdi_destroy_channel_bynum(ss7->pvts[channel]->channel);
		if (res == RESULT_SUCCESS) {
			ast_cli(a->fd, "CIC destroyed: CIC: %i DPC: %i dahdi chan: %i\n", cur_cic, dpc, channel);
			ss7->numchans--;
			for (cur_chan = channel; cur_chan < ss7->numchans; cur_chan++)
				ss7->pvts[cur_chan] = ss7->pvts[cur_chan + 1];
		}
	}
	ast_mutex_unlock(&ss7->lock);


	return CLI_SUCCESS;
}

static char *handle_ss7_add_cics(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a) {
	int linkset, cicbegin, cicend, cur_cic, res, x, cur_dahdi, chanpos, count;
	unsigned short dpc;
	struct dahdi_ss7 *ss7;
	struct dahdi_pvt *p;
	struct dahdi_pvt *working = iflist;
	struct dahdi_pvt **wend = &ifend;
	struct dahdi_pvt **wlist = &iflist;

	char fn[80];
	struct dahdi_params params;

	switch (cmd) {
	case CLI_INIT:
		e->command = "ss7 add cics";
		e->usage =
			"Usage: ss7 add cics <linkset> <cicbegin> <cicend> <dpc> <dahdi chan>\n"
			"       Destroy cics\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc < 8)
		return CLI_SHOWUSAGE;
	linkset = atoi(a->argv[3]);

	if ((linkset < 1) || (linkset > NUM_SPANS)) {
		ast_cli(a->fd, "Invalid linkset %s.  Should be a number %d to %d\n", a->argv[2], 1, NUM_SPANS);
		return CLI_SUCCESS;
	}
	if (!linksets[linkset-1].ss7) {
		ast_cli(a->fd, "No SS7 running on linkset %d\n", linkset);
		return CLI_SUCCESS;
	}

	if (linksets[linkset-1].ss7)
		ss7 = &linksets[linkset-1];

	cicbegin = atoi(a->argv[4]);
	cicend = atoi(a->argv[5]);
	dpc = atoi(a->argv[6]);
	cur_dahdi = atoi(a->argv[7]);

	if (!ss7->numchans) {
		ast_cli(a->fd, "Need at least 1 existing cic!\n");
		return CLI_SUCCESS;
	}

	if(!cicbegin || !cicend || !dpc || cicend < cicbegin || !cur_dahdi) {
		ast_cli(a->fd, "Invalid cicbegin/cicend/dpc/dahdi_chan\n");
		return CLI_SUCCESS;
	}

	for (cur_cic = cicbegin; cur_cic <= cicend; cur_cic++) {
		if (ss7_find_cic(ss7, cur_cic, dpc) != -1) {
			ast_cli(a->fd, "CIC: %i DPC: %i already exists\n", cur_cic, dpc);
			return CLI_SUCCESS;
		}
	}

	ast_mutex_lock(&iflock);
	ast_mutex_lock(&ss7->lock);
	for(cur_cic = cicbegin; cur_cic <= cicend; cur_cic++) {
		chanpos = ss7->pvts[ss7->numchans - 1]->channel;

		p = ast_calloc(1, sizeof(*p));

		memcpy(p, ss7->pvts[ss7->numchans - 1], sizeof(*p));
		ast_mutex_init(&p->lock);
		ifcount++;

		for (x = 0; x < 3; x++)
			p->subs[x].dfd = -1;
		p->channel = cur_dahdi;
		p->owner = NULL;
		p->ss7call = NULL;
		p->inservice = 0;
		p->dpc = dpc;
		p->cic = cur_cic;

		snprintf(fn, sizeof(fn), "%d", p->channel);
		p->subs[SUB_REAL].dfd = dahdi_open(fn);
		count = 0;
		while (p->subs[SUB_REAL].dfd < 0 && count < 1000) { /* the kernel may not call dahdi_release fast enough for the open flagbit to be cleared in time */
			usleep(1);
			p->subs[SUB_REAL].dfd = dahdi_open(fn);
			count++;
		}
		/* Allocate a DAHDI structure */
		if (p->subs[SUB_REAL].dfd < 0) {
			ast_log(LOG_ERROR, "Unable to open channel %d: %s\n", p->channel, strerror(errno));
			ast_free(p);
			p = NULL;
		}

		if (p) {
			res = ioctl(p->subs[SUB_REAL].dfd, DAHDI_GET_PARAMS, &params);
			if (res < 0) {
				ast_log(LOG_ERROR, "Unable to get parameters: %s\n", strerror(errno));
				ast_free(p);
				p = NULL;
			}
		}

		if (p) {
			/* check if we maybe have to put it on the begining */
			if (working->channel > p->channel) {
				p->next = *wlist;
				p->prev = NULL;
				(*wlist)->prev = p;
				*wlist = p;
			} else {
				while (working) {
					/* in the middle */
					if (working->next) {
						if (working->channel < p->channel && working->next->channel > p->channel) {
							p->next = working->next;
							p->prev = working;
							working->next->prev = p;
							working->next = p;
							break;
						}
					} else {
						/* the last */
						if (working->channel < p->channel) {
							working->next = p;
							p->next = NULL;
							p->prev = working;
							*wend = p;
							break;
						}
					}
					working = working->next;
				}
			}

			ss7->pvts[ss7->numchans] = p;
			ss7->numchans++;
			cur_dahdi++;
			ast_cli(a->fd, "Added new CIC: %i DPC: %i\n", p->cic, p->dpc);
		} else {
			ast_mutex_unlock(&ss7->lock);
			ast_mutex_unlock(&iflock);
			return CLI_SUCCESS;
		}
	}
	ast_mutex_unlock(&ss7->lock);
	ast_mutex_unlock(&iflock);

	return CLI_SUCCESS;
}


static struct ast_cli_entry dahdi_ss7_cli[] = {
	AST_CLI_DEFINE(handle_ss7_debug, "Enables SS7 debugging on a linkset"),
	AST_CLI_DEFINE(handle_ss7_no_debug, "Disables SS7 debugging on a linkset"),
	AST_CLI_DEFINE(handle_ss7_block_cic, "Blocks the given CIC"),
	AST_CLI_DEFINE(handle_ss7_reset_cic, "Resets the given CIC"),
	AST_CLI_DEFINE(handle_ss7_group_reset, "Resets the given CIC range"),
	AST_CLI_DEFINE(handle_ss7_group_block, "Blocks the given CIC range"),
	AST_CLI_DEFINE(handle_ss7_group_unblock, "Unblocks the given CIC range"),
	AST_CLI_DEFINE(handle_ss7_unblock_cic, "Unblocks the given CIC"),
	AST_CLI_DEFINE(handle_ss7_block_linkset, "Blocks all CICs on a linkset"),
	AST_CLI_DEFINE(handle_ss7_unblock_linkset, "Unblocks all CICs on a linkset"),
	AST_CLI_DEFINE(handle_ss7_show_linkset, "Shows the status of a linkset"),
	AST_CLI_DEFINE(handle_ss7_show_calls, "Show ss7 calls"),
	AST_CLI_DEFINE(handle_ss7_show_cics, "Show cics on a linkset"),
	AST_CLI_DEFINE(handle_ss7_net_mnt, "Send an NET MNT message"),
	AST_CLI_DEFINE(handle_ss7_mtp3_restart, "Restart a link"),
	AST_CLI_DEFINE(handle_ss7_destroy_cics, "Destroy ss7 cics in linkset"),
	AST_CLI_DEFINE(handle_ss7_add_cics, "Add ss7 cics in linkset")
};
#endif /* HAVE_SS7 */

static int __unload_module(void)
{
	int x;
	struct dahdi_pvt *p, *pl;
#if defined(HAVE_PRI) || defined(HAVE_SS7)
	int i, j;
#endif

#ifdef HAVE_PRI
	for (i = 0; i < NUM_SPANS; i++) {
		if (pris[i].master != AST_PTHREADT_NULL)
			pthread_cancel(pris[i].master);
	}
	ast_cli_unregister_multiple(dahdi_pri_cli, ARRAY_LEN(dahdi_pri_cli));
	ast_unregister_application(dahdi_send_keypad_facility_app);
#endif
#if defined(HAVE_SS7)
	for (i = 0; i < NUM_SPANS; i++) {
		if (linksets[i].master != AST_PTHREADT_NULL)
			pthread_cancel(linksets[i].master);
		}
	ast_cli_unregister_multiple(dahdi_ss7_cli, sizeof(dahdi_ss7_cli) / sizeof(struct ast_cli_entry));
#endif

	ast_cli_unregister_multiple(dahdi_cli, sizeof(dahdi_cli) / sizeof(struct ast_cli_entry));
	ast_manager_unregister( "DAHDIDialOffhook" );
	ast_manager_unregister( "DAHDIHangup" );
	ast_manager_unregister( "DAHDITransfer" );
	ast_manager_unregister( "DAHDIDNDoff" );
	ast_manager_unregister( "DAHDIDNDon" );
	ast_manager_unregister("DAHDIShowChannels");
	ast_manager_unregister("DAHDIRestart");
	ast_channel_unregister(&dahdi_tech);
	ast_mutex_lock(&iflock);
	/* Hangup all interfaces if they have an owner */
	p = iflist;
	while (p) {
		if (p->owner)
			ast_softhangup(p->owner, AST_SOFTHANGUP_APPUNLOAD);
		p = p->next;
	}
	ast_mutex_unlock(&iflock);
	ast_mutex_lock(&monlock);
	if (monitor_thread && (monitor_thread != AST_PTHREADT_STOP) && (monitor_thread != AST_PTHREADT_NULL)) {
		pthread_cancel(monitor_thread);
		pthread_kill(monitor_thread, SIGURG);
		pthread_join(monitor_thread, NULL);
	}
	monitor_thread = AST_PTHREADT_STOP;
	ast_mutex_unlock(&monlock);

	ast_mutex_lock(&iflock);
	/* Destroy all the interfaces and free their memory */
	p = iflist;
	while (p) {
		/* Free any callerid */
		if (p->cidspill)
			ast_free(p->cidspill);
		/* Close the DAHDI thingy */
		if (p->subs[SUB_REAL].dfd > -1)
			dahdi_close(p->subs[SUB_REAL].dfd);
		pl = p;
		p = p->next;
		x = pl->channel;
		/* Free associated memory */
		if (pl)
			destroy_dahdi_pvt(&pl);
		ast_verb(3, "Unregistered channel %d\n", x);
	}
	iflist = NULL;
	ifcount = 0;
	ast_mutex_unlock(&iflock);

#if defined(HAVE_PRI)
	for (i = 0; i < NUM_SPANS; i++) {
		if (pris[i].master && (pris[i].master != AST_PTHREADT_NULL))
			pthread_join(pris[i].master, NULL);
		for (j = 0; j < NUM_DCHANS; j++) {
			dahdi_close(pris[i].fds[j]);
		}
	}
#endif

#if defined(HAVE_SS7)
	for (i = 0; i < NUM_SPANS; i++) {
		if (linksets[i].master && (linksets[i].master != AST_PTHREADT_NULL))
			pthread_join(linksets[i].master, NULL);
		for (j = 0; j < NUM_DCHANS; j++) {
			dahdi_close(linksets[i].fds[j]);
		}
		if (linksets[i].ss7)
			ss7_destroy(linksets[i].ss7);
	}
#endif

	ast_cond_destroy(&ss_thread_complete);
	return 0;
}

static int unload_module(void)
{
#if defined(HAVE_PRI) || defined(HAVE_SS7)
	int y;
#endif
#ifdef HAVE_PRI
	for (y = 0; y < NUM_SPANS; y++)
		ast_mutex_destroy(&pris[y].lock);
#endif
#ifdef HAVE_SS7
	for (y = 0; y < NUM_SPANS; y++)
		ast_mutex_destroy(&linksets[y].lock);
#endif /* HAVE_SS7 */
	return __unload_module();
}

static int build_channels(struct dahdi_chan_conf *conf, int iscrv, const char *value, int reload, int lineno, int *found_pseudo)
{
	char *c, *chan;
	int x, start, finish;
	struct dahdi_pvt *tmp;
#ifdef HAVE_PRI
	struct dahdi_pri *pri;
	int trunkgroup, y;
#endif

	if ((reload == 0) && (conf->chan.sig < 0) && !conf->is_sig_auto) {
		ast_log(LOG_ERROR, "Signalling must be specified before any channels are.\n");
		return -1;
	}

	c = ast_strdupa(value);

#ifdef HAVE_PRI
	pri = NULL;
	if (iscrv) {
		if (sscanf(c, "%d:%n", &trunkgroup, &y) != 1) {
			ast_log(LOG_WARNING, "CRV must begin with trunkgroup followed by a colon at line %d.\n", lineno);
			return -1;
		}
		if (trunkgroup < 1) {
			ast_log(LOG_WARNING, "CRV trunk group must be a positive number at line %d.\n", lineno);
			return -1;
		}
		c += y;
		for (y = 0; y < NUM_SPANS; y++) {
			if (pris[y].trunkgroup == trunkgroup) {
				pri = pris + y;
				break;
			}
		}
		if (!pri) {
			ast_log(LOG_WARNING, "No such trunk group %d at CRV declaration at line %d.\n", trunkgroup, lineno);
			return -1;
		}
	}
#endif

	while ((chan = strsep(&c, ","))) {
		if (sscanf(chan, "%d-%d", &start, &finish) == 2) {
			/* Range */
		} else if (sscanf(chan, "%d", &start)) {
			/* Just one */
			finish = start;
		} else if (!strcasecmp(chan, "pseudo")) {
			finish = start = CHAN_PSEUDO;
			if (found_pseudo)
				*found_pseudo = 1;
		} else {
			ast_log(LOG_ERROR, "Syntax error parsing '%s' at '%s'\n", value, chan);
			return -1;
		}
		if (finish < start) {
			ast_log(LOG_WARNING, "Sillyness: %d < %d\n", start, finish);
			x = finish;
			finish = start;
			start = x;
		}

		for (x = start; x <= finish; x++) {
#ifdef HAVE_PRI
			tmp = mkintf(x, conf, pri, reload);
#else
			tmp = mkintf(x, conf, NULL, reload);
#endif

			if (tmp) {
#ifdef HAVE_PRI
					if (pri)
					ast_verb(3, "%s CRV %d:%d, %s signalling\n", reload ? "Reconfigured" : "Registered", trunkgroup, x, sig2str(tmp->sig));
					else
#endif
					ast_verb(3, "%s channel %d, %s signalling\n", reload ? "Reconfigured" : "Registered", x, sig2str(tmp->sig));
			} else {
				ast_log(LOG_ERROR, "Unable to %s channel '%s'\n",
					(reload == 1) ? "reconfigure" : "register", value);
				return -1;
			}
		}
	}

	return 0;
}

/** The length of the parameters list of 'dahdichan'.
 * \todo Move definition of MAX_CHANLIST_LEN to a proper place. */
#define MAX_CHANLIST_LEN 80

static void process_echocancel(struct dahdi_chan_conf *confp, const char *data, unsigned int line)
{
	char *parse = ast_strdupa(data);
	char *params[DAHDI_MAX_ECHOCANPARAMS + 1];
	unsigned int param_count;
	unsigned int x;

	if (!(param_count = ast_app_separate_args(parse, ',', params, ARRAY_LEN(params))))
		return;

	memset(&confp->chan.echocancel, 0, sizeof(confp->chan.echocancel));

	/* first parameter is tap length, process it here */

	x = ast_strlen_zero(params[0]) ? 0 : atoi(params[0]);

	if ((x == 32) || (x == 64) || (x == 128) || (x == 256) || (x == 512) || (x == 1024))
		confp->chan.echocancel.head.tap_length = x;
	else if ((confp->chan.echocancel.head.tap_length = ast_true(params[0])))
		confp->chan.echocancel.head.tap_length = 128;

	/* now process any remaining parameters */

	for (x = 1; x < param_count; x++) {
		struct {
			char *name;
			char *value;
		} param;

		if (ast_app_separate_args(params[x], '=', (char **) &param, 2) < 1) {
			ast_log(LOG_WARNING, "Invalid echocancel parameter supplied at line %d: '%s'\n", line, params[x]);
			continue;
		}

		if (ast_strlen_zero(param.name) || (strlen(param.name) > sizeof(confp->chan.echocancel.params[0].name)-1)) {
			ast_log(LOG_WARNING, "Invalid echocancel parameter supplied at line %d: '%s'\n", line, param.name);
			continue;
		}

		strcpy(confp->chan.echocancel.params[confp->chan.echocancel.head.param_count].name, param.name);

		if (param.value) {
			if (sscanf(param.value, "%d", &confp->chan.echocancel.params[confp->chan.echocancel.head.param_count].value) != 1) {
				ast_log(LOG_WARNING, "Invalid echocancel parameter value supplied at line %d: '%s'\n", line, param.value);
				continue;
			}
		}
		confp->chan.echocancel.head.param_count++;
	}
}


static int process_dahdi(struct dahdi_chan_conf *confp, struct ast_variable *v, int reload, int skipchannels)
{
	struct dahdi_pvt *tmp;
	int y;
	int found_pseudo = 0;
        char dahdichan[MAX_CHANLIST_LEN] = {};

	for (; v; v = v->next) {
		if (!ast_jb_read_conf(&global_jbconf, v->name, v->value))
			continue;

		/* Create the interface list */
		if (!strcasecmp(v->name, "channel")
#ifdef HAVE_PRI
		    || !strcasecmp(v->name, "crv")
#endif
			) {
 			int iscrv;
 			if (skipchannels)
 				continue;
 			iscrv = !strcasecmp(v->name, "crv");
 			if (build_channels(confp, iscrv, v->value, reload, v->lineno, &found_pseudo))
 					return -1;
		} else if (!strcasecmp(v->name, "buffers")) {
			int res;
			char policy[8] = "";
			res = sscanf(v->value, "%d,%s", &confp->chan.buf_no, policy);
			if (res != 2) {
				ast_log(LOG_WARNING, "Parsing buffers option data failed, using defaults.\n");
				confp->chan.buf_no = numbufs;
				continue;
			}
			if (confp->chan.buf_no < 0)
				confp->chan.buf_no = numbufs;
			if (!strcasecmp(policy, "full")) {
				confp->chan.buf_policy = DAHDI_POLICY_WHEN_FULL;
			} else if (!strcasecmp(policy, "half")) {
				confp->chan.buf_policy = DAHDI_POLICY_IMMEDIATE /*HALF_FULL*/;
			} else if (!strcasecmp(policy, "immediate")) {
				confp->chan.buf_policy = DAHDI_POLICY_IMMEDIATE;
			} else {
				ast_log(LOG_WARNING, "Invalid policy name given (%s).\n", policy);
			}
 		} else if (!strcasecmp(v->name, "dahdichan")) {
 			ast_copy_string(dahdichan, v->value, sizeof(dahdichan));
		} else if (!strcasecmp(v->name, "usedistinctiveringdetection")) {
			usedistinctiveringdetection = ast_true(v->value);
		} else if (!strcasecmp(v->name, "distinctiveringaftercid")) {
			distinctiveringaftercid = ast_true(v->value);
		} else if (!strcasecmp(v->name, "dring1context")) {
			ast_copy_string(confp->chan.drings.ringContext[0].contextData,v->value,sizeof(confp->chan.drings.ringContext[0].contextData));
		} else if (!strcasecmp(v->name, "dring2context")) {
			ast_copy_string(confp->chan.drings.ringContext[1].contextData,v->value,sizeof(confp->chan.drings.ringContext[1].contextData));
		} else if (!strcasecmp(v->name, "dring3context")) {
			ast_copy_string(confp->chan.drings.ringContext[2].contextData,v->value,sizeof(confp->chan.drings.ringContext[2].contextData));
		} else if (!strcasecmp(v->name, "dring1range")) {
			confp->chan.drings.ringnum[0].range = atoi(v->value);
		} else if (!strcasecmp(v->name, "dring2range")) {
			confp->chan.drings.ringnum[1].range = atoi(v->value);
		} else if (!strcasecmp(v->name, "dring3range")) {
			confp->chan.drings.ringnum[2].range = atoi(v->value);
		} else if (!strcasecmp(v->name, "dring1")) {
			sscanf(v->value, "%d,%d,%d", &confp->chan.drings.ringnum[0].ring[0], &confp->chan.drings.ringnum[0].ring[1], &confp->chan.drings.ringnum[0].ring[2]);
		} else if (!strcasecmp(v->name, "dring2")) {
			sscanf(v->value,"%d,%d,%d", &confp->chan.drings.ringnum[1].ring[0], &confp->chan.drings.ringnum[1].ring[1], &confp->chan.drings.ringnum[1].ring[2]);
		} else if (!strcasecmp(v->name, "dring3")) {
			sscanf(v->value, "%d,%d,%d", &confp->chan.drings.ringnum[2].ring[0], &confp->chan.drings.ringnum[2].ring[1], &confp->chan.drings.ringnum[2].ring[2]);
		} else if (!strcasecmp(v->name, "usecallerid")) {
			confp->chan.use_callerid = ast_true(v->value);
		} else if (!strcasecmp(v->name, "cidsignalling")) {
			if (!strcasecmp(v->value, "bell"))
				confp->chan.cid_signalling = CID_SIG_BELL;
			else if (!strcasecmp(v->value, "v23"))
				confp->chan.cid_signalling = CID_SIG_V23;
			else if (!strcasecmp(v->value, "dtmf"))
				confp->chan.cid_signalling = CID_SIG_DTMF;
			else if (!strcasecmp(v->value, "smdi"))
				confp->chan.cid_signalling = CID_SIG_SMDI;
			else if (!strcasecmp(v->value, "v23_jp"))
				confp->chan.cid_signalling = CID_SIG_V23_JP;
			else if (ast_true(v->value))
				confp->chan.cid_signalling = CID_SIG_BELL;
		} else if (!strcasecmp(v->name, "cidstart")) {
			if (!strcasecmp(v->value, "ring"))
				confp->chan.cid_start = CID_START_RING;
			else if (!strcasecmp(v->value, "polarity_in"))
				confp->chan.cid_start = CID_START_POLARITY_IN;
			else if (!strcasecmp(v->value, "polarity"))
				confp->chan.cid_start = CID_START_POLARITY;
			else if (ast_true(v->value))
				confp->chan.cid_start = CID_START_RING;
		} else if (!strcasecmp(v->name, "threewaycalling")) {
			confp->chan.threewaycalling = ast_true(v->value);
		} else if (!strcasecmp(v->name, "cancallforward")) {
			confp->chan.cancallforward = ast_true(v->value);
		} else if (!strcasecmp(v->name, "relaxdtmf")) {
			if (ast_true(v->value))
				confp->chan.dtmfrelax = DSP_DIGITMODE_RELAXDTMF;
			else
				confp->chan.dtmfrelax = 0;
		} else if (!strcasecmp(v->name, "mailbox")) {
			ast_copy_string(confp->chan.mailbox, v->value, sizeof(confp->chan.mailbox));
		} else if (!strcasecmp(v->name, "adsi")) {
			confp->chan.adsi = ast_true(v->value);
		} else if (!strcasecmp(v->name, "usesmdi")) {
			confp->chan.use_smdi = ast_true(v->value);
		} else if (!strcasecmp(v->name, "smdiport")) {
			ast_copy_string(confp->smdi_port, v->value, sizeof(confp->smdi_port));
		} else if (!strcasecmp(v->name, "transfer")) {
			confp->chan.transfer = ast_true(v->value);
		} else if (!strcasecmp(v->name, "canpark")) {
			confp->chan.canpark = ast_true(v->value);
		} else if (!strcasecmp(v->name, "echocancelwhenbridged")) {
			confp->chan.echocanbridged = ast_true(v->value);
		} else if (!strcasecmp(v->name, "busydetect")) {
			confp->chan.busydetect = ast_true(v->value);
		} else if (!strcasecmp(v->name, "busycount")) {
			confp->chan.busycount = atoi(v->value);
		} else if (!strcasecmp(v->name, "busypattern")) {
			if (sscanf(v->value, "%d,%d", &confp->chan.busy_tonelength, &confp->chan.busy_quietlength) != 2) {
				ast_log(LOG_ERROR, "busypattern= expects busypattern=tonelength,quietlength at line %d.\n", v->lineno);
			}
		} else if (!strcasecmp(v->name, "callprogress")) {
			confp->chan.callprogress &= ~CALLPROGRESS_PROGRESS;
			if (ast_true(v->value))
				confp->chan.callprogress |= CALLPROGRESS_PROGRESS;
		} else if (!strcasecmp(v->name, "faxdetect")) {
			confp->chan.callprogress &= ~CALLPROGRESS_FAX;
			if (!strcasecmp(v->value, "incoming")) {
				confp->chan.callprogress |= CALLPROGRESS_FAX_INCOMING;
			} else if (!strcasecmp(v->value, "outgoing")) {
				confp->chan.callprogress |= CALLPROGRESS_FAX_OUTGOING;
			} else if (!strcasecmp(v->value, "both") || ast_true(v->value))
				confp->chan.callprogress |= CALLPROGRESS_FAX_INCOMING | CALLPROGRESS_FAX_OUTGOING;
		} else if (!strcasecmp(v->name, "echocancel")) {
			process_echocancel(confp, v->value, v->lineno);
		} else if (!strcasecmp(v->name, "echotraining")) {
			if (sscanf(v->value, "%d", &y) == 1) {
				if ((y < 10) || (y > 4000)) {
					ast_log(LOG_WARNING, "Echo training time must be within the range of 10 to 4000 ms at line %d.\n", v->lineno);
				} else {
					confp->chan.echotraining = y;
				}
			} else if (ast_true(v->value)) {
				confp->chan.echotraining = 400;
			} else
				confp->chan.echotraining = 0;
		} else if (!strcasecmp(v->name, "hidecallerid")) {
			confp->chan.hidecallerid = ast_true(v->value);
		} else if (!strcasecmp(v->name, "hidecalleridname")) {
			confp->chan.hidecalleridname = ast_true(v->value);
 		} else if (!strcasecmp(v->name, "pulsedial")) {
 			confp->chan.pulse = ast_true(v->value);
		} else if (!strcasecmp(v->name, "callreturn")) {
			confp->chan.callreturn = ast_true(v->value);
		} else if (!strcasecmp(v->name, "callwaiting")) {
			confp->chan.callwaiting = ast_true(v->value);
		} else if (!strcasecmp(v->name, "callwaitingcallerid")) {
			confp->chan.callwaitingcallerid = ast_true(v->value);
		} else if (!strcasecmp(v->name, "context")) {
			ast_copy_string(confp->chan.context, v->value, sizeof(confp->chan.context));
		} else if (!strcasecmp(v->name, "language")) {
			ast_copy_string(confp->chan.language, v->value, sizeof(confp->chan.language));
		} else if (!strcasecmp(v->name, "progzone")) {
			ast_copy_string(progzone, v->value, sizeof(progzone));
		} else if (!strcasecmp(v->name, "mohinterpret")
			||!strcasecmp(v->name, "musiconhold") || !strcasecmp(v->name, "musicclass")) {
			ast_copy_string(confp->chan.mohinterpret, v->value, sizeof(confp->chan.mohinterpret));
		} else if (!strcasecmp(v->name, "mohsuggest")) {
			ast_copy_string(confp->chan.mohsuggest, v->value, sizeof(confp->chan.mohsuggest));
		} else if (!strcasecmp(v->name, "stripmsd")) {
			ast_log(LOG_NOTICE, "Configuration option \"%s\" has been deprecated. Please use dialplan instead\n", v->name);
			confp->chan.stripmsd = atoi(v->value);
		} else if (!strcasecmp(v->name, "jitterbuffers")) {
			numbufs = atoi(v->value);
		} else if (!strcasecmp(v->name, "group")) {
			confp->chan.group = ast_get_group(v->value);
		} else if (!strcasecmp(v->name, "callgroup")) {
			if (!strcasecmp(v->value, "none"))
				confp->chan.callgroup = 0;
			else
				confp->chan.callgroup = ast_get_group(v->value);
		} else if (!strcasecmp(v->name, "pickupgroup")) {
			if (!strcasecmp(v->value, "none"))
				confp->chan.pickupgroup = 0;
			else
				confp->chan.pickupgroup = ast_get_group(v->value);
		} else if (!strcasecmp(v->name, "setvar")) {
			char *varname = ast_strdupa(v->value), *varval = NULL;
			struct ast_variable *tmpvar;
			if (varname && (varval = strchr(varname, '='))) {
				*varval++ = '\0';
				if ((tmpvar = ast_variable_new(varname, varval, ""))) {
					tmpvar->next = confp->chan.vars;
					confp->chan.vars = tmpvar;
				}
			}
		} else if (!strcasecmp(v->name, "immediate")) {
			confp->chan.immediate = ast_true(v->value);
		} else if (!strcasecmp(v->name, "transfertobusy")) {
			confp->chan.transfertobusy = ast_true(v->value);
		} else if (!strcasecmp(v->name, "mwimonitor")) {
			if (!strcasecmp(v->value, "neon")) {
				confp->chan.mwimonitor_neon = 1;
				confp->chan.mwimonitor_fsk = 0;
			} else {
				confp->chan.mwimonitor_neon = 0;
				if (!strcasecmp(v->value, "fsk"))
					confp->chan.mwimonitor_fsk = 1;
				else
					confp->chan.mwimonitor_fsk = ast_true(v->value) ? 1 : 0;
			}
		} else if (!strcasecmp(v->name, "cid_rxgain")) {
			if (sscanf(v->value, "%f", &confp->chan.cid_rxgain) != 1) {
				ast_log(LOG_WARNING, "Invalid cid_rxgain: %s at line %d.\n", v->value, v->lineno);
			}
		} else if (!strcasecmp(v->name, "rxgain")) {
			if (sscanf(v->value, "%f", &confp->chan.rxgain) != 1) {
				ast_log(LOG_WARNING, "Invalid rxgain: %s at line %d.\n", v->value, v->lineno);
			}
		} else if (!strcasecmp(v->name, "txgain")) {
			if (sscanf(v->value, "%f", &confp->chan.txgain) != 1) {
				ast_log(LOG_WARNING, "Invalid txgain: %s at line %d.\n", v->value, v->lineno);
			}
		} else if (!strcasecmp(v->name, "tonezone")) {
			if (sscanf(v->value, "%d", &confp->chan.tonezone) != 1) {
				ast_log(LOG_WARNING, "Invalid tonezone: %s at line %d.\n", v->value, v->lineno);
			}
		} else if (!strcasecmp(v->name, "callerid")) {
			if (!strcasecmp(v->value, "asreceived")) {
				confp->chan.cid_num[0] = '\0';
				confp->chan.cid_name[0] = '\0';
			} else {
				ast_callerid_split(v->value, confp->chan.cid_name, sizeof(confp->chan.cid_name), confp->chan.cid_num, sizeof(confp->chan.cid_num));
			}
		} else if (!strcasecmp(v->name, "fullname")) {
			ast_copy_string(confp->chan.cid_name, v->value, sizeof(confp->chan.cid_name));
		} else if (!strcasecmp(v->name, "cid_number")) {
			ast_copy_string(confp->chan.cid_num, v->value, sizeof(confp->chan.cid_num));
		} else if (!strcasecmp(v->name, "useincomingcalleridondahditransfer")) {
			confp->chan.dahditrcallerid = ast_true(v->value);
		} else if (!strcasecmp(v->name, "restrictcid")) {
			confp->chan.restrictcid = ast_true(v->value);
		} else if (!strcasecmp(v->name, "usecallingpres")) {
			confp->chan.use_callingpres = ast_true(v->value);
		} else if (!strcasecmp(v->name, "accountcode")) {
			ast_copy_string(confp->chan.accountcode, v->value, sizeof(confp->chan.accountcode));
		} else if (!strcasecmp(v->name, "amaflags")) {
			y = ast_cdr_amaflags2int(v->value);
			if (y < 0)
				ast_log(LOG_WARNING, "Invalid AMA flags: %s at line %d.\n", v->value, v->lineno);
			else
				confp->chan.amaflags = y;
		} else if (!strcasecmp(v->name, "polarityonanswerdelay")) {
			confp->chan.polarityonanswerdelay = atoi(v->value);
		} else if (!strcasecmp(v->name, "answeronpolarityswitch")) {
			confp->chan.answeronpolarityswitch = ast_true(v->value);
		} else if (!strcasecmp(v->name, "hanguponpolarityswitch")) {
			confp->chan.hanguponpolarityswitch = ast_true(v->value);
		} else if (!strcasecmp(v->name, "sendcalleridafter")) {
			confp->chan.sendcalleridafter = atoi(v->value);
		} else if (!strcasecmp(v->name, "mwimonitornotify")) {
			ast_copy_string(mwimonitornotify, v->value, sizeof(mwimonitornotify));
		} else if (reload != 1) {
			 if (!strcasecmp(v->name, "signalling") || !strcasecmp(v->name, "signaling")) {
				int orig_radio = confp->chan.radio;
				int orig_outsigmod = confp->chan.outsigmod;
				int orig_auto = confp->is_sig_auto;

				confp->chan.radio = 0;
				confp->chan.outsigmod = -1;
				confp->is_sig_auto = 0;
				if (!strcasecmp(v->value, "em")) {
					confp->chan.sig = SIG_EM;
				} else if (!strcasecmp(v->value, "em_e1")) {
					confp->chan.sig = SIG_EM_E1;
				} else if (!strcasecmp(v->value, "em_w")) {
					confp->chan.sig = SIG_EMWINK;
				} else if (!strcasecmp(v->value, "fxs_ls")) {
					confp->chan.sig = SIG_FXSLS;
				} else if (!strcasecmp(v->value, "fxs_gs")) {
					confp->chan.sig = SIG_FXSGS;
				} else if (!strcasecmp(v->value, "fxs_ks")) {
					confp->chan.sig = SIG_FXSKS;
				} else if (!strcasecmp(v->value, "fxo_ls")) {
					confp->chan.sig = SIG_FXOLS;
				} else if (!strcasecmp(v->value, "fxo_gs")) {
					confp->chan.sig = SIG_FXOGS;
				} else if (!strcasecmp(v->value, "fxo_ks")) {
					confp->chan.sig = SIG_FXOKS;
				} else if (!strcasecmp(v->value, "fxs_rx")) {
					confp->chan.sig = SIG_FXSKS;
					confp->chan.radio = 1;
				} else if (!strcasecmp(v->value, "fxo_rx")) {
					confp->chan.sig = SIG_FXOLS;
					confp->chan.radio = 1;
				} else if (!strcasecmp(v->value, "fxs_tx")) {
					confp->chan.sig = SIG_FXSLS;
					confp->chan.radio = 1;
				} else if (!strcasecmp(v->value, "fxo_tx")) {
					confp->chan.sig = SIG_FXOGS;
					confp->chan.radio = 1;
				} else if (!strcasecmp(v->value, "em_rx")) {
					confp->chan.sig = SIG_EM;
					confp->chan.radio = 1;
				} else if (!strcasecmp(v->value, "em_tx")) {
					confp->chan.sig = SIG_EM;
					confp->chan.radio = 1;
				} else if (!strcasecmp(v->value, "em_rxtx")) {
					confp->chan.sig = SIG_EM;
					confp->chan.radio = 2;
				} else if (!strcasecmp(v->value, "em_txrx")) {
					confp->chan.sig = SIG_EM;
					confp->chan.radio = 2;
				} else if (!strcasecmp(v->value, "sf")) {
					confp->chan.sig = SIG_SF;
				} else if (!strcasecmp(v->value, "sf_w")) {
					confp->chan.sig = SIG_SFWINK;
				} else if (!strcasecmp(v->value, "sf_featd")) {
					confp->chan.sig = SIG_FEATD;
				} else if (!strcasecmp(v->value, "sf_featdmf")) {
					confp->chan.sig = SIG_FEATDMF;
				} else if (!strcasecmp(v->value, "sf_featb")) {
					confp->chan.sig = SIG_SF_FEATB;
				} else if (!strcasecmp(v->value, "sf")) {
					confp->chan.sig = SIG_SF;
				} else if (!strcasecmp(v->value, "sf_rx")) {
					confp->chan.sig = SIG_SF;
					confp->chan.radio = 1;
				} else if (!strcasecmp(v->value, "sf_tx")) {
					confp->chan.sig = SIG_SF;
					confp->chan.radio = 1;
				} else if (!strcasecmp(v->value, "sf_rxtx")) {
					confp->chan.sig = SIG_SF;
					confp->chan.radio = 2;
				} else if (!strcasecmp(v->value, "sf_txrx")) {
					confp->chan.sig = SIG_SF;
					confp->chan.radio = 2;
				} else if (!strcasecmp(v->value, "featd")) {
					confp->chan.sig = SIG_FEATD;
				} else if (!strcasecmp(v->value, "featdmf")) {
					confp->chan.sig = SIG_FEATDMF;
				} else if (!strcasecmp(v->value, "featdmf_ta")) {
					confp->chan.sig = SIG_FEATDMF_TA;
				} else if (!strcasecmp(v->value, "e911")) {
					confp->chan.sig = SIG_E911;
				} else if (!strcasecmp(v->value, "fgccama")) {
					confp->chan.sig = SIG_FGC_CAMA;
				} else if (!strcasecmp(v->value, "fgccamamf")) {
					confp->chan.sig = SIG_FGC_CAMAMF;
				} else if (!strcasecmp(v->value, "featb")) {
					confp->chan.sig = SIG_FEATB;
#ifdef HAVE_PRI
				} else if (!strcasecmp(v->value, "pri_net")) {
					confp->chan.sig = SIG_PRI;
					confp->pri.nodetype = PRI_NETWORK;
				} else if (!strcasecmp(v->value, "pri_cpe")) {
					confp->chan.sig = SIG_PRI;
					confp->pri.nodetype = PRI_CPE;
				} else if (!strcasecmp(v->value, "bri_cpe")) {
					confp->chan.sig = SIG_BRI;
					confp->pri.nodetype = PRI_CPE;
				} else if (!strcasecmp(v->value, "bri_net")) {
					confp->chan.sig = SIG_BRI;
					confp->pri.nodetype = PRI_NETWORK;
				} else if (!strcasecmp(v->value, "bri_cpe_ptmp")) {
					confp->chan.sig = SIG_BRI_PTMP;
					confp->pri.nodetype = PRI_CPE;
				} else if (!strcasecmp(v->value, "bri_net_ptmp")) {
					ast_log(LOG_WARNING, "How cool would it be if someone implemented this mode!  For now, sucks for you. (line %d)\n", v->lineno);
				} else if (!strcasecmp(v->value, "gr303fxoks_net")) {
					confp->chan.sig = SIG_GR303FXOKS;
					confp->pri.nodetype = PRI_NETWORK;
				} else if (!strcasecmp(v->value, "gr303fxsks_cpe")) {
					confp->chan.sig = SIG_GR303FXSKS;
					confp->pri.nodetype = PRI_CPE;
#endif
#ifdef HAVE_SS7
				} else if (!strcasecmp(v->value, "ss7")) {
					confp->chan.sig = SIG_SS7;
#endif
				} else if (!strcasecmp(v->value, "auto")) {
					confp->is_sig_auto = 1;
				} else {
					confp->chan.outsigmod = orig_outsigmod;
					confp->chan.radio = orig_radio;
					confp->is_sig_auto = orig_auto;
					ast_log(LOG_ERROR, "Unknown signalling method '%s' at line %d.\n", v->value, v->lineno);
				}
			 } else if (!strcasecmp(v->name, "outsignalling") || !strcasecmp(v->name, "outsignaling")) {
				if (!strcasecmp(v->value, "em")) {
					confp->chan.outsigmod = SIG_EM;
				} else if (!strcasecmp(v->value, "em_e1")) {
					confp->chan.outsigmod = SIG_EM_E1;
				} else if (!strcasecmp(v->value, "em_w")) {
					confp->chan.outsigmod = SIG_EMWINK;
				} else if (!strcasecmp(v->value, "sf")) {
					confp->chan.outsigmod = SIG_SF;
				} else if (!strcasecmp(v->value, "sf_w")) {
					confp->chan.outsigmod = SIG_SFWINK;
				} else if (!strcasecmp(v->value, "sf_featd")) {
					confp->chan.outsigmod = SIG_FEATD;
				} else if (!strcasecmp(v->value, "sf_featdmf")) {
					confp->chan.outsigmod = SIG_FEATDMF;
				} else if (!strcasecmp(v->value, "sf_featb")) {
					confp->chan.outsigmod = SIG_SF_FEATB;
				} else if (!strcasecmp(v->value, "sf")) {
					confp->chan.outsigmod = SIG_SF;
				} else if (!strcasecmp(v->value, "featd")) {
					confp->chan.outsigmod = SIG_FEATD;
				} else if (!strcasecmp(v->value, "featdmf")) {
					confp->chan.outsigmod = SIG_FEATDMF;
				} else if (!strcasecmp(v->value, "featdmf_ta")) {
					confp->chan.outsigmod = SIG_FEATDMF_TA;
				} else if (!strcasecmp(v->value, "e911")) {
					confp->chan.outsigmod = SIG_E911;
				} else if (!strcasecmp(v->value, "fgccama")) {
					confp->chan.outsigmod = SIG_FGC_CAMA;
				} else if (!strcasecmp(v->value, "fgccamamf")) {
					confp->chan.outsigmod = SIG_FGC_CAMAMF;
				} else if (!strcasecmp(v->value, "featb")) {
					confp->chan.outsigmod = SIG_FEATB;
				} else {
					ast_log(LOG_ERROR, "Unknown signalling method '%s' at line %d.\n", v->value, v->lineno);
				}
#ifdef HAVE_PRI
			} else if (!strcasecmp(v->name, "pridialplan")) {
				if (!strcasecmp(v->value, "national")) {
					confp->pri.dialplan = PRI_NATIONAL_ISDN + 1;
				} else if (!strcasecmp(v->value, "unknown")) {
					confp->pri.dialplan = PRI_UNKNOWN + 1;
				} else if (!strcasecmp(v->value, "private")) {
					confp->pri.dialplan = PRI_PRIVATE + 1;
				} else if (!strcasecmp(v->value, "international")) {
					confp->pri.dialplan = PRI_INTERNATIONAL_ISDN + 1;
				} else if (!strcasecmp(v->value, "local")) {
					confp->pri.dialplan = PRI_LOCAL_ISDN + 1;
	 			} else if (!strcasecmp(v->value, "dynamic")) {
 					confp->pri.dialplan = -1;
				} else if (!strcasecmp(v->value, "redundant")) {
					confp->pri.dialplan = -2;
				} else {
					ast_log(LOG_WARNING, "Unknown PRI dialplan '%s' at line %d.\n", v->value, v->lineno);
				}
			} else if (!strcasecmp(v->name, "prilocaldialplan")) {
				if (!strcasecmp(v->value, "national")) {
					confp->pri.localdialplan = PRI_NATIONAL_ISDN + 1;
				} else if (!strcasecmp(v->value, "unknown")) {
					confp->pri.localdialplan = PRI_UNKNOWN + 1;
				} else if (!strcasecmp(v->value, "private")) {
					confp->pri.localdialplan = PRI_PRIVATE + 1;
				} else if (!strcasecmp(v->value, "international")) {
					confp->pri.localdialplan = PRI_INTERNATIONAL_ISDN + 1;
				} else if (!strcasecmp(v->value, "local")) {
					confp->pri.localdialplan = PRI_LOCAL_ISDN + 1;
				} else if (!strcasecmp(v->value, "dynamic")) {
					confp->pri.localdialplan = -1;
				} else if (!strcasecmp(v->value, "redundant")) {
					confp->pri.localdialplan = -2;
				} else {
					ast_log(LOG_WARNING, "Unknown PRI localdialplan '%s' at line %d.\n", v->value, v->lineno);
				}
			} else if (!strcasecmp(v->name, "switchtype")) {
				if (!strcasecmp(v->value, "national"))
					confp->pri.switchtype = PRI_SWITCH_NI2;
				else if (!strcasecmp(v->value, "ni1"))
					confp->pri.switchtype = PRI_SWITCH_NI1;
				else if (!strcasecmp(v->value, "dms100"))
					confp->pri.switchtype = PRI_SWITCH_DMS100;
				else if (!strcasecmp(v->value, "4ess"))
					confp->pri.switchtype = PRI_SWITCH_ATT4ESS;
				else if (!strcasecmp(v->value, "5ess"))
					confp->pri.switchtype = PRI_SWITCH_LUCENT5E;
				else if (!strcasecmp(v->value, "euroisdn"))
					confp->pri.switchtype = PRI_SWITCH_EUROISDN_E1;
				else if (!strcasecmp(v->value, "qsig"))
					confp->pri.switchtype = PRI_SWITCH_QSIG;
				else {
					ast_log(LOG_ERROR, "Unknown switchtype '%s' at line %d.\n", v->value, v->lineno);
					return -1;
				}
			} else if (!strcasecmp(v->name, "nsf")) {
				if (!strcasecmp(v->value, "sdn"))
					confp->pri.nsf = PRI_NSF_SDN;
				else if (!strcasecmp(v->value, "megacom"))
					confp->pri.nsf = PRI_NSF_MEGACOM;
				else if (!strcasecmp(v->value, "tollfreemegacom"))
					confp->pri.nsf = PRI_NSF_TOLL_FREE_MEGACOM;
				else if (!strcasecmp(v->value, "accunet"))
					confp->pri.nsf = PRI_NSF_ACCUNET;
				else if (!strcasecmp(v->value, "none"))
					confp->pri.nsf = PRI_NSF_NONE;
				else {
					ast_log(LOG_WARNING, "Unknown network-specific facility '%s' at line %d.\n", v->value, v->lineno);
					confp->pri.nsf = PRI_NSF_NONE;
				}
			} else if (!strcasecmp(v->name, "priindication")) {
				if (!strcasecmp(v->value, "outofband"))
					confp->chan.priindication_oob = 1;
				else if (!strcasecmp(v->value, "inband"))
					confp->chan.priindication_oob = 0;
				else
					ast_log(LOG_WARNING, "'%s' is not a valid pri indication value, should be 'inband' or 'outofband' at line %d.\n",
						v->value, v->lineno);
			} else if (!strcasecmp(v->name, "priexclusive")) {
				confp->chan.priexclusive = ast_true(v->value);
			} else if (!strcasecmp(v->name, "internationalprefix")) {
				ast_copy_string(confp->pri.internationalprefix, v->value, sizeof(confp->pri.internationalprefix));
			} else if (!strcasecmp(v->name, "nationalprefix")) {
				ast_copy_string(confp->pri.nationalprefix, v->value, sizeof(confp->pri.nationalprefix));
			} else if (!strcasecmp(v->name, "localprefix")) {
				ast_copy_string(confp->pri.localprefix, v->value, sizeof(confp->pri.localprefix));
			} else if (!strcasecmp(v->name, "privateprefix")) {
				ast_copy_string(confp->pri.privateprefix, v->value, sizeof(confp->pri.privateprefix));
			} else if (!strcasecmp(v->name, "unknownprefix")) {
				ast_copy_string(confp->pri.unknownprefix, v->value, sizeof(confp->pri.unknownprefix));
			} else if (!strcasecmp(v->name, "resetinterval")) {
				if (!strcasecmp(v->value, "never"))
					confp->pri.resetinterval = -1;
				else if (atoi(v->value) >= 60)
					confp->pri.resetinterval = atoi(v->value);
				else
					ast_log(LOG_WARNING, "'%s' is not a valid reset interval, should be >= 60 seconds or 'never' at line %d.\n",
						v->value, v->lineno);
			} else if (!strcasecmp(v->name, "minunused")) {
				confp->pri.minunused = atoi(v->value);
			} else if (!strcasecmp(v->name, "minidle")) {
				confp->pri.minidle = atoi(v->value);
			} else if (!strcasecmp(v->name, "idleext")) {
				ast_copy_string(confp->pri.idleext, v->value, sizeof(confp->pri.idleext));
			} else if (!strcasecmp(v->name, "idledial")) {
				ast_copy_string(confp->pri.idledial, v->value, sizeof(confp->pri.idledial));
			} else if (!strcasecmp(v->name, "overlapdial")) {
				if (ast_true(v->value)) {
					confp->pri.overlapdial = DAHDI_OVERLAPDIAL_BOTH;
				} else if (!strcasecmp(v->value, "incoming")) {
					confp->pri.overlapdial = DAHDI_OVERLAPDIAL_INCOMING;
				} else if (!strcasecmp(v->value, "outgoing")) {
					confp->pri.overlapdial = DAHDI_OVERLAPDIAL_OUTGOING;
				} else if (!strcasecmp(v->value, "both") || ast_true(v->value)) {
					confp->pri.overlapdial = DAHDI_OVERLAPDIAL_BOTH;
				} else {
					confp->pri.overlapdial = DAHDI_OVERLAPDIAL_NONE;
				}
#ifdef HAVE_PRI_INBANDDISCONNECT
			} else if (!strcasecmp(v->name, "inbanddisconnect")) {
				confp->pri.inbanddisconnect = ast_true(v->value);
#endif
			} else if (!strcasecmp(v->name, "pritimer")) {
#ifdef PRI_GETSET_TIMERS
				char tmp[20], *timerc, *c = tmp;
				int timer, timeridx;
				ast_copy_string(tmp, v->value, sizeof(tmp));
				timerc = strsep(&c, ",");
				if (timerc) {
					timer = atoi(c);
					if (!timer)
						ast_log(LOG_WARNING, "'%s' is not a valid value for an ISDN timer at line %d.\n", timerc, v->lineno);
					else {
						if ((timeridx = pri_timer2idx(timerc)) >= 0)
							pritimers[timeridx] = timer;
						else
							ast_log(LOG_WARNING, "'%s' is not a valid ISDN timer at line %d.\n", timerc, v->lineno);
					}
				} else
					ast_log(LOG_WARNING, "'%s' is not a valid ISDN timer configuration string at line %d.\n", v->value, v->lineno);

			} else if (!strcasecmp(v->name, "facilityenable")) {
				confp->pri.facilityenable = ast_true(v->value);
#endif /* PRI_GETSET_TIMERS */
#endif /* HAVE_PRI */
#ifdef HAVE_SS7
			} else if (!strcasecmp(v->name, "ss7type")) {
				if (!strcasecmp(v->value, "itu")) {
					cur_ss7type = SS7_ITU;
				} else if (!strcasecmp(v->value, "ansi")) {
					cur_ss7type = SS7_ANSI;
				} else
					ast_log(LOG_WARNING, "'%s' is an unknown ss7 switch type at line %d.!\n", v->value, v->lineno);
			} else if (!strcasecmp(v->name, "linkset")) {
				cur_linkset = atoi(v->value);
			} else if (!strcasecmp(v->name, "pointcode")) {
				cur_pointcode = parse_pointcode(v->value);
			} else if (!strcasecmp(v->name, "adjpointcode")) {
				cur_adjpointcode = parse_pointcode(v->value);
			} else if (!strcasecmp(v->name, "defaultdpc")) {
				cur_defaultdpc = parse_pointcode(v->value);
			} else if (!strcasecmp(v->name, "cicbeginswith")) {
				cur_cicbeginswith = atoi(v->value);
			} else if (!strcasecmp(v->name, "networkindicator")) {
				if (!strcasecmp(v->value, "national"))
					cur_networkindicator = SS7_NI_NAT;
				else if (!strcasecmp(v->value, "national_spare"))
					cur_networkindicator = SS7_NI_NAT_SPARE;
				else if (!strcasecmp(v->value, "international"))
					cur_networkindicator = SS7_NI_INT;
				else if (!strcasecmp(v->value, "international_spare"))
					cur_networkindicator = SS7_NI_INT_SPARE;
				else
					cur_networkindicator = -1;
			} else if (!strcasecmp(v->name, "ss7_internationalprefix")) {
				ast_copy_string(confp->ss7.internationalprefix, v->value, sizeof(confp->ss7.internationalprefix));
			} else if (!strcasecmp(v->name, "ss7_nationalprefix")) {
				ast_copy_string(confp->ss7.nationalprefix, v->value, sizeof(confp->ss7.nationalprefix));
			} else if (!strcasecmp(v->name, "ss7_subscriberprefix")) {
				ast_copy_string(confp->ss7.subscriberprefix, v->value, sizeof(confp->ss7.subscriberprefix));
			} else if (!strcasecmp(v->name, "ss7_unknownprefix")) {
				ast_copy_string(confp->ss7.unknownprefix, v->value, sizeof(confp->ss7.unknownprefix));
			} else if (!strcasecmp(v->name, "ss7_networkroutedprefix")) {
				ast_copy_string(confp->ss7.networkroutedprefix, v->value, sizeof(confp->ss7.networkroutedprefix));
			} else if (!strcasecmp(v->name, "ss7_called_nai")) {
				if (!strcasecmp(v->value, "national")) {
					confp->ss7.called_nai = SS7_NAI_NATIONAL;
				} else if (!strcasecmp(v->value, "international")) {
					confp->ss7.called_nai = SS7_NAI_INTERNATIONAL;
				} else if (!strcasecmp(v->value, "subscriber")) {
					confp->ss7.called_nai = SS7_NAI_SUBSCRIBER;
	 			} else if (!strcasecmp(v->value, "dynamic")) {
 					confp->ss7.called_nai = SS7_NAI_DYNAMIC;
				} else {
					ast_log(LOG_WARNING, "Unknown SS7 called_nai '%s' at line %d.\n", v->value, v->lineno);
				}
			} else if (!strcasecmp(v->name, "ss7_calling_nai")) {
				if (!strcasecmp(v->value, "national")) {
					confp->ss7.calling_nai = SS7_NAI_NATIONAL;
				} else if (!strcasecmp(v->value, "international")) {
					confp->ss7.calling_nai = SS7_NAI_INTERNATIONAL;
				} else if (!strcasecmp(v->value, "subscriber")) {
					confp->ss7.calling_nai = SS7_NAI_SUBSCRIBER;
				} else if (!strcasecmp(v->value, "dynamic")) {
					confp->ss7.calling_nai = SS7_NAI_DYNAMIC;
				} else {
					ast_log(LOG_WARNING, "Unknown SS7 calling_nai '%s' at line %d.\n", v->value, v->lineno);
				}
			} else if (!strcasecmp(v->name, "sigchan")) {
				int sigchan, res;
				sigchan = atoi(v->value);
				res = linkset_addsigchan(sigchan);
				if (res < 0)
					return -1;

			} else if (!strcasecmp(v->name, "ss7_explicitacm")) {
				struct dahdi_ss7 *link;
				link = ss7_resolve_linkset(cur_linkset);
				if (!link) {
					ast_log(LOG_ERROR, "Invalid linkset number.  Must be between 1 and %d\n", NUM_SPANS + 1);
					return -1;
				}
				if (ast_true(v->value))
					link->flags |= LINKSET_FLAG_EXPLICITACM;
			} else if (!strcasecmp(v->name, "ss7_initialhwblo")) {
				struct dahdi_ss7 *link;
				link = ss7_resolve_linkset(cur_linkset);
				if (!link) {
					ast_log(LOG_ERROR, "Invalid linkset number.  Must be between 1 and %d\n", NUM_SPANS + 1);
					return -1;
				}
				if (ast_true(v->value))
					link->flags |= LINKSET_FLAG_INITIALHWBLO;
			} else if (!strcasecmp(v->name, "ss7_use_echocontrol")) {
				struct dahdi_ss7 *link;
				link = ss7_resolve_linkset(cur_linkset);
				if (!link) {
					ast_log(LOG_ERROR, "Invalid linkset number.  Must be between 1 and %d\n", NUM_SPANS + 1);
					return -1;
				}
				if (ast_true(v->value))
					link->flags |= LINKSET_FLAG_USEECHOCONTROL;
			} else if (!strcasecmp(v->name, "ss7_default_echocontrol")) {
				struct dahdi_ss7 *link;
				link = ss7_resolve_linkset(cur_linkset);
				if (!link) {
					ast_log(LOG_ERROR, "Invalid linkset number.  Must be between 1 and %d\n", NUM_SPANS + 1);
					return -1;
				}
				if (ast_true(v->value))
					link->flags |= LINKSET_FLAG_DEFAULTECHOCONTROL;
			} else if (!strncasecmp(v->name, "isup_timer", 10)) {
				struct dahdi_ss7 *link;
				link = ss7_resolve_linkset(cur_linkset);
				if (!link) {
					ast_log(LOG_ERROR, "Invalid linkset number.  Must be between 1 and %d\n", NUM_SPANS + 1);
					return -1;
				}
				if(!link->ss7)
					ast_log(LOG_ERROR, "Please specify isup timers after sigchan!\n");
				else if(!ss7_set_isup_timer(link->ss7, strstr(v->name, ".") + 1, atoi(v->value)))
					ast_log(LOG_ERROR, "Invalid isup timer %s\n", v->name);
			} else if (!strncasecmp(v->name, "mtp3_timer", 10)) {
				struct dahdi_ss7 *link;
				link = ss7_resolve_linkset(cur_linkset);
				if (!link) {
					ast_log(LOG_ERROR, "Invalid linkset number.  Must be between 1 and %d\n", NUM_SPANS + 1);
					return -1;
				}
				if(!link->ss7)
					ast_log(LOG_ERROR, "Please specify mtp3 timers after sigchan!\n");
				else if(!ss7_set_mtp3_timer(link->ss7, strstr(v->name, ".") + 1, atoi(v->value)))
					ast_log(LOG_ERROR, "Invalid mtp3 timer %s\n", v->name);
			} else if (!strcasecmp(v->name, "inr_if_no_calling")) {
				struct dahdi_ss7 *link;
				link = ss7_resolve_linkset(cur_linkset);
				if (!link) {
					ast_log(LOG_ERROR, "Invalid linkset number.  Must be between 1 and %d\n", NUM_SPANS + 1);
					return -1;
				}
				if(!link->ss7)
					ast_log(LOG_ERROR, "Please specify inr_if_no_calling after sigchan!\n");
				else if (ast_true(v->value))
					ss7_set_flags(link->ss7, SS7_INR_IF_NO_CALLING);
			} else if (!strcasecmp(v->name, "non_isdn_access")) {
				struct dahdi_ss7 *link;
				link = ss7_resolve_linkset(cur_linkset);
				if (!link) {
					ast_log(LOG_ERROR, "Invalid linkset number.  Must be between 1 and %d\n", NUM_SPANS + 1);
					return -1;
				}
				if(!link->ss7)
					ast_log(LOG_ERROR, "Please specify non_isdn_access after sigchan!\n");
				else if (ast_true(v->value))
					ss7_clear_flags(link->ss7, SS7_ISDN_ACCES_INDICATOR);
			} else if (!strcasecmp(v->name, "sls_shift")) {
				struct dahdi_ss7 *link;
				link = ss7_resolve_linkset(cur_linkset);
				if (!link) {
					ast_log(LOG_ERROR, "Invalid linkset number.  Must be between 1 and %d\n", NUM_SPANS + 1);
					return -1;
				}
				if(!link->ss7)
					ast_log(LOG_ERROR, "Please specify sls_shift after sigchan!\n");
				else
					ss7_set_sls_shift(link->ss7, atoi(v->value));
#endif /* HAVE_SS7 */
			} else if (!strcasecmp(v->name, "cadence")) {
				/* setup to scan our argument */
				int element_count, c[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
				int i;
				struct dahdi_ring_cadence new_cadence;
				int cid_location = -1;
				int firstcadencepos = 0;
				char original_args[80];
				int cadence_is_ok = 1;

				ast_copy_string(original_args, v->value, sizeof(original_args));
				/* 16 cadences allowed (8 pairs) */
				element_count = sscanf(v->value, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", &c[0], &c[1], &c[2], &c[3], &c[4], &c[5], &c[6], &c[7], &c[8], &c[9], &c[10], &c[11], &c[12], &c[13], &c[14], &c[15]);

				/* Cadence must be even (on/off) */
				if (element_count % 2 == 1) {
					ast_log(LOG_ERROR, "Must be a silence duration for each ring duration: %s at line %d.\n", original_args, v->lineno);
					cadence_is_ok = 0;
				}

				/* Ring cadences cannot be negative */
				for (i = 0; i < element_count; i++) {
					if (c[i] == 0) {
						ast_log(LOG_ERROR, "Ring or silence duration cannot be zero: %s at line %d.\n", original_args, v->lineno);
						cadence_is_ok = 0;
						break;
					} else if (c[i] < 0) {
						if (i % 2 == 1) {
							/* Silence duration, negative possibly okay */
							if (cid_location == -1) {
								cid_location = i;
								c[i] *= -1;
							} else {
								ast_log(LOG_ERROR, "CID location specified twice: %s at line %d.\n", original_args, v->lineno);
								cadence_is_ok = 0;
								break;
							}
						} else {
							if (firstcadencepos == 0) {
								firstcadencepos = i; /* only recorded to avoid duplicate specification */
											/* duration will be passed negative to the DAHDI driver */
							} else {
								 ast_log(LOG_ERROR, "First cadence position specified twice: %s at line %d.\n", original_args, v->lineno);
								cadence_is_ok = 0;
								break;
							}
						}
					}
				}

				/* Substitute our scanned cadence */
				for (i = 0; i < 16; i++) {
					new_cadence.ringcadence[i] = c[i];
				}

				if (cadence_is_ok) {
					/* ---we scanned it without getting annoyed; now some sanity checks--- */
					if (element_count < 2) {
						ast_log(LOG_ERROR, "Minimum cadence is ring,pause: %s at line %d.\n", original_args, v->lineno);
					} else {
						if (cid_location == -1) {
							/* user didn't say; default to first pause */
							cid_location = 1;
						} else {
							/* convert element_index to cidrings value */
							cid_location = (cid_location + 1) / 2;
						}
						/* ---we like their cadence; try to install it--- */
						if (!user_has_defined_cadences++)
							/* this is the first user-defined cadence; clear the default user cadences */
							num_cadence = 0;
						if ((num_cadence+1) >= NUM_CADENCE_MAX)
							ast_log(LOG_ERROR, "Already %d cadences; can't add another: %s at line %d.\n", NUM_CADENCE_MAX, original_args, v->lineno);
						else {
							cadences[num_cadence] = new_cadence;
							cidrings[num_cadence++] = cid_location;
							ast_verb(3, "cadence 'r%d' added: %s\n",num_cadence,original_args);
						}
					}
				}
			} else if (!strcasecmp(v->name, "ringtimeout")) {
				ringt_base = (atoi(v->value) * 8) / READ_SIZE;
			} else if (!strcasecmp(v->name, "prewink")) {
				confp->timing.prewinktime = atoi(v->value);
			} else if (!strcasecmp(v->name, "preflash")) {
				confp->timing.preflashtime = atoi(v->value);
			} else if (!strcasecmp(v->name, "wink")) {
				confp->timing.winktime = atoi(v->value);
			} else if (!strcasecmp(v->name, "flash")) {
				confp->timing.flashtime = atoi(v->value);
			} else if (!strcasecmp(v->name, "start")) {
				confp->timing.starttime = atoi(v->value);
			} else if (!strcasecmp(v->name, "rxwink")) {
				confp->timing.rxwinktime = atoi(v->value);
			} else if (!strcasecmp(v->name, "rxflash")) {
				confp->timing.rxflashtime = atoi(v->value);
			} else if (!strcasecmp(v->name, "debounce")) {
				confp->timing.debouncetime = atoi(v->value);
			} else if (!strcasecmp(v->name, "toneduration")) {
				int toneduration;
				int ctlfd;
				int res;
				struct dahdi_dialparams dps;

				ctlfd = open("/dev/dahdi/ctl", O_RDWR);
				if (ctlfd == -1) {
					ast_log(LOG_ERROR, "Unable to open /dev/dahdi/ctl to set toneduration at line %d.\n", v->lineno);
					return -1;
				}

				toneduration = atoi(v->value);
				if (toneduration > -1) {
					memset(&dps, 0, sizeof(dps));

					dps.dtmf_tonelen = dps.mfv1_tonelen = toneduration;
					res = ioctl(ctlfd, DAHDI_SET_DIALPARAMS, &dps);
					if (res < 0) {
						ast_log(LOG_ERROR, "Invalid tone duration: %d ms at line %d: %s\n", toneduration, v->lineno, strerror(errno));
						return -1;
					}
				}
				close(ctlfd);
			} else if (!strcasecmp(v->name, "defaultcic")) {
				ast_copy_string(defaultcic, v->value, sizeof(defaultcic));
			} else if (!strcasecmp(v->name, "defaultozz")) {
				ast_copy_string(defaultozz, v->value, sizeof(defaultozz));
			} else if (!strcasecmp(v->name, "mwilevel")) {
				mwilevel = atoi(v->value);
			}
		} else if (!skipchannels)
			ast_log(LOG_WARNING, "Ignoring any changes to '%s' (on reload) at line %d.\n", v->name, v->lineno);
	}
	if (dahdichan[0]) {
		/* The user has set 'dahdichan' */
		/*< \todo pass proper line number instead of 0 */
		if (build_channels(confp, 0, dahdichan, reload, 0, &found_pseudo)) {
			return -1;
		}
	}
	/*< \todo why check for the pseudo in the per-channel section.
	 * Any actual use for manual setup of the pseudo channel? */
	if (!found_pseudo && reload == 0) {
		/* Make sure pseudo isn't a member of any groups if
		   we're automatically making it. */

		confp->chan.group = 0;
		confp->chan.callgroup = 0;
		confp->chan.pickupgroup = 0;

		tmp = mkintf(CHAN_PSEUDO, confp, NULL, reload);

		if (tmp) {
			ast_verb(3, "Automatically generated pseudo channel\n");
		} else {
			ast_log(LOG_WARNING, "Unable to register pseudo channel!\n");
		}
	}
	return 0;
}

static int setup_dahdi(int reload)
{
	struct ast_config *cfg, *ucfg;
	struct ast_variable *v;
 	struct dahdi_chan_conf base_conf = dahdi_chan_conf_default();
 	struct dahdi_chan_conf conf;
	struct ast_flags config_flags = { reload == 1 ? CONFIG_FLAG_FILEUNCHANGED : 0 };
	int res;

#ifdef HAVE_PRI
	char *c;
	int spanno;
	int i;
	int logicalspan;
	int trunkgroup;
	int dchannels[NUM_DCHANS];
#endif

	cfg = ast_config_load(config, config_flags);

	/* Error if we have no config file */
	if (!cfg) {
		ast_log(LOG_ERROR, "Unable to load config %s\n", config);
		return 0;
	} else if (cfg == CONFIG_STATUS_FILEUNCHANGED) {
		ucfg = ast_config_load("users.conf", config_flags);
		if (ucfg == CONFIG_STATUS_FILEUNCHANGED)
			return 0;
		ast_clear_flag(&config_flags, CONFIG_FLAG_FILEUNCHANGED);
		cfg = ast_config_load(config, config_flags);
	} else {
		ast_clear_flag(&config_flags, CONFIG_FLAG_FILEUNCHANGED);
		ucfg = ast_config_load("users.conf", config_flags);
	}

	/* It's a little silly to lock it, but we mind as well just to be sure */
	ast_mutex_lock(&iflock);
#ifdef HAVE_PRI
	if (reload != 1) {
		/* Process trunkgroups first */
		v = ast_variable_browse(cfg, "trunkgroups");
		while (v) {
			if (!strcasecmp(v->name, "trunkgroup")) {
				trunkgroup = atoi(v->value);
				if (trunkgroup > 0) {
					if ((c = strchr(v->value, ','))) {
						i = 0;
						memset(dchannels, 0, sizeof(dchannels));
						while (c && (i < NUM_DCHANS)) {
							dchannels[i] = atoi(c + 1);
							if (dchannels[i] < 0) {
								ast_log(LOG_WARNING, "D-channel for trunk group %d must be a postiive number at line %d of chan_dahdi.conf\n", trunkgroup, v->lineno);
							} else
								i++;
							c = strchr(c + 1, ',');
						}
						if (i) {
							if (pri_create_trunkgroup(trunkgroup, dchannels)) {
								ast_log(LOG_WARNING, "Unable to create trunk group %d with Primary D-channel %d at line %d of chan_dahdi.conf\n", trunkgroup, dchannels[0], v->lineno);
						} else
								ast_verb(2, "Created trunk group %d with Primary D-channel %d and %d backup%s\n", trunkgroup, dchannels[0], i - 1, (i == 1) ? "" : "s");
						} else
							ast_log(LOG_WARNING, "Trunk group %d lacks any valid D-channels at line %d of chan_dahdi.conf\n", trunkgroup, v->lineno);
					} else
						ast_log(LOG_WARNING, "Trunk group %d lacks a primary D-channel at line %d of chan_dahdi.conf\n", trunkgroup, v->lineno);
				} else
					ast_log(LOG_WARNING, "Trunk group identifier must be a positive integer at line %d of chan_dahdi.conf\n", v->lineno);
			} else if (!strcasecmp(v->name, "spanmap")) {
				spanno = atoi(v->value);
				if (spanno > 0) {
					if ((c = strchr(v->value, ','))) {
						trunkgroup = atoi(c + 1);
						if (trunkgroup > 0) {
							if ((c = strchr(c + 1, ',')))
								logicalspan = atoi(c + 1);
							else
								logicalspan = 0;
							if (logicalspan >= 0) {
								if (pri_create_spanmap(spanno - 1, trunkgroup, logicalspan)) {
									ast_log(LOG_WARNING, "Failed to map span %d to trunk group %d (logical span %d)\n", spanno, trunkgroup, logicalspan);
							} else
									ast_verb(2, "Mapped span %d to trunk group %d (logical span %d)\n", spanno, trunkgroup, logicalspan);
							} else
								ast_log(LOG_WARNING, "Logical span must be a postive number, or '0' (for unspecified) at line %d of chan_dahdi.conf\n", v->lineno);
						} else
							ast_log(LOG_WARNING, "Trunk group must be a postive number at line %d of chan_dahdi.conf\n", v->lineno);
					} else
						ast_log(LOG_WARNING, "Missing trunk group for span map at line %d of chan_dahdi.conf\n", v->lineno);
				} else
					ast_log(LOG_WARNING, "Span number must be a postive integer at line %d of chan_dahdi.conf\n", v->lineno);
			} else {
				ast_log(LOG_NOTICE, "Ignoring unknown keyword '%s' in trunkgroups\n", v->name);
			}
			v = v->next;
		}
	}
#endif

	/* Copy the default jb config over global_jbconf */
	memcpy(&global_jbconf, &default_jbconf, sizeof(struct ast_jb_conf));

	mwimonitornotify[0] = '\0';

	v = ast_variable_browse(cfg, "channels");
	res = process_dahdi(&base_conf, v, reload, 0);
	ast_mutex_unlock(&iflock);
	ast_config_destroy(cfg);
	if (res)
		return res;
	if (ucfg) {
		char *cat;
		const char *chans;
		process_dahdi(&base_conf, ast_variable_browse(ucfg, "general"), 1, 1);
		for (cat = ast_category_browse(ucfg, NULL); cat ; cat = ast_category_browse(ucfg, cat)) {
			if (!strcasecmp(cat, "general"))
				continue;
			chans = ast_variable_retrieve(ucfg, cat, "dahdichan");
			if (!ast_strlen_zero(chans)) {
				if (memcpy(&conf, &base_conf, sizeof(conf)) == NULL) {
					ast_log(LOG_ERROR, "Not enough memory for conf copy\n");
					ast_config_destroy(ucfg);
					return -1;
				}
				process_dahdi(&conf, ast_variable_browse(ucfg, cat), reload, 0);
			}
		}
		ast_config_destroy(ucfg);
	}
#ifdef HAVE_PRI
	if (reload != 1) {
		int x;
		for (x = 0; x < NUM_SPANS; x++) {
			if (pris[x].pvts[0]) {
				if (start_pri(pris + x)) {
					ast_log(LOG_ERROR, "Unable to start D-channel on span %d\n", x + 1);
					return -1;
				} else
					ast_verb(2, "Starting D-Channel on span %d\n", x + 1);
			}
		}
	}
#endif
#ifdef HAVE_SS7
	if (reload != 1) {
		int x;
		for (x = 0; x < NUM_SPANS; x++) {
			if (linksets[x].ss7) {
				if (ast_pthread_create(&linksets[x].master, NULL, ss7_linkset, &linksets[x])) {
					ast_log(LOG_ERROR, "Unable to start SS7 linkset on span %d\n", x + 1);
					return -1;
				} else
					ast_verb(2, "Starting SS7 linkset on span %d\n", x + 1);
			}
		}
	}
#endif
	/* And start the monitor for the first time */
	restart_monitor();
	return 0;
}

static int load_module(void)
{
	int res;
#if defined(HAVE_PRI) || defined(HAVE_SS7)
	int y, i;
#endif

#ifdef HAVE_PRI
	memset(pris, 0, sizeof(pris));
	for (y = 0; y < NUM_SPANS; y++) {
		ast_mutex_init(&pris[y].lock);
		pris[y].offset = -1;
		pris[y].master = AST_PTHREADT_NULL;
		for (i = 0; i < NUM_DCHANS; i++)
			pris[y].fds[i] = -1;
	}
	pri_set_error(dahdi_pri_error);
	pri_set_message(dahdi_pri_message);
	ast_register_application(dahdi_send_keypad_facility_app, dahdi_send_keypad_facility_exec,
			dahdi_send_keypad_facility_synopsis, dahdi_send_keypad_facility_descrip);
#endif
#ifdef HAVE_SS7
	memset(linksets, 0, sizeof(linksets));
	for (y = 0; y < NUM_SPANS; y++) {
		ast_mutex_init(&linksets[y].lock);
		linksets[y].master = AST_PTHREADT_NULL;
		for (i = 0; i < NUM_DCHANS; i++)
			linksets[y].fds[i] = -1;
	}
	ss7_set_error(dahdi_ss7_error);
	ss7_set_message(dahdi_ss7_message);
	ss7_set_hangup(dahdi_ss7_hangup);
	ss7_set_notinservice(dahdi_ss7_notinservice);
	ss7_set_call_null(dahdi_ss7_call_null);
#endif /* HAVE_SS7 */
	res = setup_dahdi(0);
	/* Make sure we can register our DAHDI channel type */
	if (res)
		return AST_MODULE_LOAD_DECLINE;
	if (ast_channel_register(&dahdi_tech)) {
		ast_log(LOG_ERROR, "Unable to register channel class 'DAHDI'\n");
		__unload_module();
		return AST_MODULE_LOAD_FAILURE;
	}
#ifdef HAVE_PRI
	ast_string_field_init(&inuse, 16);
	ast_string_field_set(&inuse, name, "GR-303InUse");
	ast_cli_register_multiple(dahdi_pri_cli, sizeof(dahdi_pri_cli) / sizeof(struct ast_cli_entry));
#endif
#ifdef HAVE_SS7
	ast_cli_register_multiple(dahdi_ss7_cli, ARRAY_LEN(dahdi_ss7_cli));
#endif

	ast_cli_register_multiple(dahdi_cli, sizeof(dahdi_cli) / sizeof(struct ast_cli_entry));

	memset(round_robin, 0, sizeof(round_robin));
	ast_manager_register("DAHDITransfer", 0, action_transfer, "Transfer DAHDI Channel" );
	ast_manager_register("DAHDIHangup", 0, action_transferhangup, "Hangup DAHDI Channel" );
	ast_manager_register("DAHDIDialOffhook", 0, action_dahdidialoffhook, "Dial over DAHDI channel while offhook" );
	ast_manager_register("DAHDIDNDon", 0, action_dahdidndon, "Toggle DAHDI channel Do Not Disturb status ON" );
	ast_manager_register("DAHDIDNDoff", 0, action_dahdidndoff, "Toggle DAHDI channel Do Not Disturb status OFF" );
	ast_manager_register("DAHDIShowChannels", 0, action_dahdishowchannels, "Show status dahdi channels");
	ast_manager_register("DAHDIRestart", 0, action_dahdirestart, "Fully Restart DAHDI channels (terminates calls)");

	ast_cond_init(&ss_thread_complete, NULL);

	return res;
}

static int dahdi_sendtext(struct ast_channel *c, const char *text)
{
#define	END_SILENCE_LEN 400
#define	HEADER_MS 50
#define	TRAILER_MS 5
#define	HEADER_LEN ((HEADER_MS + TRAILER_MS) * 8)
#define	ASCII_BYTES_PER_CHAR 80

	unsigned char *buf,*mybuf;
	struct dahdi_pvt *p = c->tech_pvt;
	struct pollfd fds[1];
	int size,res,fd,len,x;
	int bytes=0;
	/* Initial carrier (imaginary) */
	float cr = 1.0;
	float ci = 0.0;
	float scont = 0.0;
	int index;

	index = dahdi_get_index(c, p, 0);
	if (index < 0) {
		ast_log(LOG_WARNING, "Huh?  I don't exist?\n");
		return -1;
	}
	if (!text[0]) return(0); /* if nothing to send, dont */
	if ((!p->tdd) && (!p->mate)) return(0);  /* if not in TDD mode, just return */
	if (p->mate)
		buf = ast_malloc(((strlen(text) + 1) * ASCII_BYTES_PER_CHAR) + END_SILENCE_LEN + HEADER_LEN);
	else
		buf = ast_malloc(((strlen(text) + 1) * TDD_BYTES_PER_CHAR) + END_SILENCE_LEN);
	if (!buf)
		return -1;
	mybuf = buf;
	if (p->mate) {
		int codec = AST_LAW(p);
		for (x = 0; x < HEADER_MS; x++) {	/* 50 ms of Mark */
			PUT_CLID_MARKMS;
		}
		/* Put actual message */
		for (x = 0; text[x]; x++) {
			PUT_CLID(text[x]);
		}
		for (x = 0; x < TRAILER_MS; x++) {	/* 5 ms of Mark */
			PUT_CLID_MARKMS;
		}
		len = bytes;
		buf = mybuf;
	} else {
		len = tdd_generate(p->tdd, buf, text);
		if (len < 1) {
			ast_log(LOG_ERROR, "TDD generate (len %d) failed!!\n", (int)strlen(text));
			ast_free(mybuf);
			return -1;
		}
	}
	memset(buf + len, 0x7f, END_SILENCE_LEN);
	len += END_SILENCE_LEN;
	fd = p->subs[index].dfd;
	while (len) {
		if (ast_check_hangup(c)) {
			ast_free(mybuf);
			return -1;
		}
		size = len;
		if (size > READ_SIZE)
			size = READ_SIZE;
		fds[0].fd = fd;
		fds[0].events = POLLOUT | POLLPRI;
		fds[0].revents = 0;
		res = poll(fds, 1, -1);
		if (!res) {
			ast_debug(1, "poll (for write) ret. 0 on channel %d\n", p->channel);
			continue;
		}
		  /* if got exception */
		if (fds[0].revents & POLLPRI) {
			ast_free(mybuf);
			return -1;
		}
		if (!(fds[0].revents & POLLOUT)) {
			ast_debug(1, "write fd not ready on channel %d\n", p->channel);
			continue;
		}
		res = write(fd, buf, size);
		if (res != size) {
			if (res == -1) {
				ast_free(mybuf);
				return -1;
			}
			ast_debug(1, "Write returned %d (%s) on channel %d\n", res, strerror(errno), p->channel);
			break;
		}
		len -= size;
		buf += size;
	}
	ast_free(mybuf);
	return(0);
}


static int reload(void)
{
	int res = 0;

	res = setup_dahdi(1);
	if (res) {
		ast_log(LOG_WARNING, "Reload of chan_dahdi.so is unsuccessful!\n");
		return -1;
	}
	return 0;
}

/* This is a workaround so that menuselect displays a proper description
 * AST_MODULE_INFO(, , "DAHDI Telephony"
 */

#ifdef HAVE_PRI
#ifdef HAVE_SS7
#define tdesc "DAHDI Telephony w/PRI & SS7"
#else
#define tdesc "DAHDI Telephony w/PRI"
#endif
#else
#ifdef HAVE_SS7
#define tdesc "DAHDI Telephony w/SS7"
#else
#define tdesc "DAHDI Telephony"
#endif
#endif

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, tdesc,
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	       );


