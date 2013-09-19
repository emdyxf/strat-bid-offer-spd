#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <map>
#include <vector>
#include <string>
#include <set>
#include <functional>
#include <iostream>
using namespace FlexAppRules;
using namespace std;

#define FLEX_OMRULE_BO_SPD	"BOSP"

#define strcmpi strcasecmp
#define stricmp strcasecmp

#define LEG_QUOTE		"QUOTE"
#define LEG_HEDGE		"HEDGE"
#define LEGID_MIN_BO		1
#define LEGID_MAX_BO		4
#define LEGID_BO_BQ		1
#define LEGID_BO_BH		2
#define	LEGID_BO_OQ		3
#define LEGID_BO_OH		4
static const double g_dEpsilon = 0.0001;


#ifdef PLATFORM_WIN
	#include "logger.h"
        #include "customer.h"
#else
	#include "/export/home/cdomegax/flexapp/flex/config/customer.h"
	#include "/export/home/cdomegax/flexapp/flex/config/logger.h"
#endif

// ###### ******** Controlling the way the Dialog opens ******* ###### // =========== START ===========
static bool IsStratInEditMode = false;
static bool IsEditFromInit = false;
// ###### ******** Controlling the way the Dialog opens ******* ###### //  ========== STOP ===========

#define STRAT_STATUS_INIT		"Init"
#define START_STATUS_PAUSE		"Paused"
#define STRAT_STATUS_WORKING		"Working"
#define STRAT_STATUS_CXLD		"Cancelled"
#define STRAT_STATUS_EDITED		"Edited"
#define STRAT_STATUS_STOP		"Stopped"

#define STRAT_MODE_ALWAYS		"Always"
#define STRAT_MODE_CONDITIONAL		"Conditional"
#define STRAT_MODE_AGGRESSIVE		"Aggressive"

#define MODEID_INVALID		0
#define MODEID_ALWAYS		1
#define MODEID_CONDITIONAL	2
#define MODEID_AGGRESSIVE	3


// Define FIX tags for BidOffer Spread: Common [9500-9502] + Specific [9505-9516, 9521-9523]
#ifndef FIX_TAG_STRATEGYNAME
#define FIX_TAG_STRATEGYNAME		9500
#endif
#ifndef FIX_TAG_DESTINATION
#define FIX_TAG_DESTINATION		9501
#endif
#ifndef FIX_TAG_OMUSER
#define FIX_TAG_OMUSER			9502
#endif

#ifndef _BOSPD_FIXTAGS_H_
#define _BOSPD_FIXTAGS_H_
#define FIX_TAG_QUOTEMODE_BO		9503
#define FIX_TAG_STRATID_BO		9504
#define FIX_TAG_STRATPORT_BO		9505

#define FIX_TAG_BENCHMARK_BO		9507
#define FIX_TAG_STRATSYM_BO		9508
#define FIX_TAG_RUNNING_BO		9509
#define FIX_TAG_ORDQTY_BO		9510
#define FIX_TAG_MAXPOS_BO		9511
#define FIX_TAG_LOTSIZE_BO		9512
#define FIX_TAG_LEGID_BO		9513
#define FIX_TAG_PAYUPTICK_BO		9514
#define FIX_TAG_MAXRPL_BO		9515
#define FIX_TAG_MAXLOSS_BO		9516

#define	FIX_TAG_STGMAXCOUNT_BO		9521
#define FIX_TAG_STGSPDINTERVAL_BO	9522
#endif // #ifndef _BOSPD_FIXTAGS_H_


// SHFE DF/OR Specific: Column names
#ifndef _FUTURE_OR_CLMNAMES_
#define _FUTURE_OR_CLMNAMES_
#define CLMNAME_TICK_SIZE		"TICK_SIZE"
#define CLMNAME_AVAIL			"AVAIL"
#define CLMNAME_LOT_SIZE		"LOT_SIZE"
#define CLMNAME_ERRCODE_FEED		"ERRCODE_FEED"
#define CLMNAME_MULTIPLIER_FEED		"MULTI_FEED"
#define CLMNAME_POS_YD_LONG		"POS_YD_LONG"
#define CLMNAME_POS_TD_LONG		"POS_TD_LONG"
#define CLMNAME_POS_YD_SHORT		"POS_YD_SHORT"
#define CLMNAME_POS_TD_SHORT		"POS_TD_SHORT"
#define CLMNAME_POS_ABS			"POS_ABS"
#define CLMNAME_POS_LIMIT		"POS_LIMIT"
#define CLMNAME_L_MARGIN_R		"L_MARGIN_R"
#define CLMNAME_S_MARGIN_R		"S_MARGIN_R"
#define CLMNNAME_UNDERLYING		"UNDERLYING"
#define CLMNAME_ROUND_LOT		"ROUND_LOT"
#define CLMNAME_ROUND_PRICE		"ROUND_PRICE"
#define CLMNAME_AVAIL_YD_LONG		"AVAIL_YD_LNG"
#define CLMNAME_AVAIL_TD_LONG		"AVAIL_TD_LNG"
#define CLMNAME_AVAIL_YD_SHORT		"AVAIL_YD_SHT"
#define CLMNAME_AVAIL_TD_SHORT		"AVAIL_TD_SHT"
#endif // ifndef _FUTURE_OR_CLMNAMES_


// SHFE DF/OR Specific: FLID values
#ifndef _FUTURE_OR_FLIDS_
#define _FUTURE_OR_FLIDS_
const int FLID_TICK_SIZE		= 16;
const int FLID_AVAIL			= 256;
const int FLID_LONG_MARGIN_RATE		= 262;
const int FLID_SHORT_MARGIN_RATE	= 263;
const int FLID_LOT_SIZE			= 524;
const int FLID_ERRCODE_FEED		= 770;
const int FLID_MULTIPLIER_FEED		= 771;
const int FLID_ORD_TYPE_SUPPORT		= 772;
const int FLID_POS_YD_LONG		= 790;
const int FLID_POS_TD_LONG		= 791;
const int FLID_POS_YD_SHORT		= 792;
const int FLID_POS_TD_SHORT		= 793;
const int FLID_POS_ABS			= 796;
const int FLID_POS_LIMIT		= 800;
const int FLID_UNDERLYING		= 1130;
const int FLID_MIN_LIMIT_ORD_SIZE	= 801;
const int FLID_MAX_LIMIT_ORD_SIZE	= 802;
const int FLID_MIN_MKT_ORD_SIZE		= 803;
const int FLID_MAX_MKT_ORD_SIZE		= 804;
const int FLID_AVAIL_YD_LONG		= 805;
const int FLID_AVAIL_TD_LONG		= 806;
const int FLID_AVAIL_YD_SHORT		= 807;
const int FLID_AVAIL_TD_SHORT		= 808;
#endif //#ifndef _FUTURE_OR_FLIDS_


#define CLIENT_MODE (0) // 1 = Client Mode; 0 = Loopback setup
#define SPD_SYM_DIFF_PORT (1) // 1 = Put the third (spread) symbol in a different portfolio linked to the original one

#define LADDER_TEST
#ifdef LADDER_TEST
	#define LADDER_TESTMODE (1)
#else
	#define LADDER_TESTMODE (0)
#endif

/* Parameters for Ladder*/
const long LADDER_ORD = 42;


/* Constants used to identify Exchange names */
// The Data exchange refers to the name of the exchange obtained in the sym-data flid
// The Send exchanges have to be maintained as "SHFE": They are assumed in the algo side to be those
const char * SHFE_DATA_EXCHANGE = "SHFE"; // Assumption?? Or Get from Market Data
const char * SHFE_CUSTOM_EXCHANGE = "SHFE"; // Used while passing parameters
const char * SHFE_SEND_EXCHANGE = "SHFE"; // Used only in VALID_ORDER


// In Lots
#ifndef _MAX_POST_SIZE_DEF_
#define _MAX_POST_SIZE_DEF_
const int MAX_POST_SIZE = 5;
#endif // ifndef _MAX_POST_SIZE_DEF_


const char * FixedMWPort()
{
	static char szPort[32] ="";
	if(szPort[0] == '\0')
		sprintf(szPort,"WatchList_%s",TraderName());
	return szPort;
}

const long SPD_DUMMY_ORD_ID = 1331;
const char * GLOBAL_DELIMITER_STRING = "-";
const char GLOBAL_DELIMITER_CHAR = '-';

const char * ORD_TYPES[] = {"MKT", "LIM", "IOC", "NONE"};

// Ladder
typedef enum{
	enColumn1 = 33,
	enColumn2 = 16,
	enColumn3 = 2,
	enColumn4 = 15,
	enColumn5 = 34,
	enInvalidColumn = -1
}SS_COLUMN_NUMBER;

typedef enum{
	enSingleClick,
	enDoubleClick,
	enRightClick,
	enMiddleClick,
	enShiftLeftClick,
	enCtrlLeftClick,
	enOtherClick
}SS_MOUSE_CLICK;

std::string strSpdLadderPort = "";
bool bIsSpdOrderFiringPending = false;
std::string strGlobalMemo = "";


/*
 *List of ENUMS : Gold, Silver, Copper and CSI300
 */

typedef enum enCMInstrType
{
        enCopper,
        enGold,
        enSilver,
        enCSI300,
        enNoInstr
};


enCMInstrType GetInstrType(int i)
{
        switch(i)
        {
                case 0:
                        return enCopper;
                case 1:
                        return enGold;
                case 2:
                        return enSilver;
                case 3:
                		return enCSI300;
                default:
                        return enNoInstr;
        }
}

const char* GetProductName(enCMInstrType eType)
{
        static const char* p[5] = {"COPPER","GOLD", "SILVER", "CSI300", "NONE"};
        switch(eType)
        {
                case enCopper:
                        return p[0];
                case enGold:
                        return p[1];
                case enSilver:
                        return p[2];
                case enCSI300:
                		return p[3];
                default:
                        return p[4];
        }
        return p[4];
}


const char* GetSHFEProductSymbol(enCMInstrType eType)
{
        static const char* p[5] = {"CU","AU", "AG", "IF", "NONE"};
        switch(eType)
        {
                case enCopper:
                        return p[0];
                case enGold:
                        return p[1];
                case enSilver:
                        return p[2];
                case enCSI300:
                		return p[3];
                default:
                        return p[4];
        }
        return p[4];
}


enCMInstrType GetInstrType(const char* pszSymbol)
{
        if(!pszSymbol)
                return enNoInstr;

        if(strstr(pszSymbol, "AU"))
                return enGold;
        else if(strstr(pszSymbol, "CU"))
                return enCopper;
        else if(strstr(pszSymbol, "AG"))
                return enSilver;
        else if(strstr(pszSymbol, "IF"))
                return enCSI300;
        return enNoInstr;
}

enCMInstrType GetInstrTypeFromProduct(const char* pszProduct)
{
        if(!pszProduct)
                return enNoInstr;

        if(!strcmp(pszProduct,"GOLD"))
                return enGold;
        else if(!strcmp(pszProduct,"COPPER"))
                return enCopper;
        else if(!strcmp(pszProduct,"SILVER"))
                return enSilver;
        else if(!strcmp(pszProduct,"CSI300"))
                return enCSI300;
        return enNoInstr;
}

const char* GetProductFromSymbol(const char* pszSymbol)
{
        return GetProductName(GetInstrType(pszSymbol));
}


/*
 * Create WatchList
 */

void LoadCommodityWatchList(int iInstrType, int nWatchMonth, int iCurrentDate)
{
	int iCurrentYear = 0, iCurrentMonth = 0, iStartMonth=0, iMonth=0;
	static enCMInstrType eInstrType = enNoInstr;
	char szExpiry[32] = "", szCommodity[32] = "";

	eInstrType = GetInstrType(iInstrType);
	sprintf(szCommodity,GetSHFEProductSymbol(eInstrType));

	iCurrentYear = iCurrentDate/10000;
	iCurrentMonth = (iCurrentDate/100)%100;
	iStartMonth = int ((iCurrentDate%100)/16);

	for(int i=iStartMonth;i<nWatchMonth+iStartMonth; i++)
	{
		iMonth = iCurrentMonth + i;
		if(iMonth > 12)
		{
			iMonth %= 12;
			sprintf(szExpiry, "%s%02d%02d", szCommodity,(iCurrentYear+1)%100,iMonth%12);
		}
		else
		{
			sprintf(szExpiry, "%s%02d%02d", szCommodity,iCurrentYear%100,iMonth);
		}
		Add_Symbol_Buy(FixedMWPort(), szExpiry, 0);
	}
}

// Calculate Day of the Week
// Tondering's algorithm
int DayOfWeek (int y, int m, int d)
{
	static int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
	y -= m < 3;
	return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}

void LoadCSI300WatchList(int iCurrentDate)
{
	int iCurrentYear = 0, iCurrentMonth = 0, iDOWFirstDay=0, iDeliveryDay=0;
	int iStartMonth=0, iMonth=0, iQuarter=0;
	static enCMInstrType eInstrType = enNoInstr;
	char szExpiry[32] = "", szCommodity[32] = "";

	int iInstrType = 3;
	eInstrType = GetInstrType(iInstrType);
	sprintf(szCommodity,GetSHFEProductSymbol(eInstrType));

	iCurrentYear = iCurrentDate/10000;
	iCurrentMonth = (iCurrentDate/100)%100;
	iDOWFirstDay = DayOfWeek(iCurrentYear, iCurrentMonth, 1);
	iDeliveryDay = iDOWFirstDay<5? (20-iDOWFirstDay) : 21;
	iStartMonth = int ( (iCurrentDate%100)>iDeliveryDay ? 1 : 0);

	// Current month and next month
	for(int i=iStartMonth;i<2+iStartMonth; i++)
	{
		iMonth = iCurrentMonth + i;
		if(iMonth > 12)
		{
			iMonth %= 12;
			sprintf(szExpiry, "%s%02d%02d", szCommodity,(iCurrentYear+1)%100,iMonth%12);
		}
		else
		{
			sprintf(szExpiry, "%s%02d%02d", szCommodity,iCurrentYear%100,iMonth);
		}
		Add_Symbol_Buy(FixedMWPort(), szExpiry, 0);
	}

	// Next two calendar quarters
	iQuarter = (iCurrentMonth+iStartMonth+1)/3;
	for (int i=1; i<3; i++)
	{
		iMonth = (iQuarter+i)*3;
		if(iMonth > 12)
		{
			iMonth %= 12;
			sprintf(szExpiry, "%s%02d%02d", szCommodity,(iCurrentYear+1)%100,iMonth%12);
		}
		else
		{
			sprintf(szExpiry, "%s%02d%02d", szCommodity,iCurrentYear%100,iMonth);
		}
		Add_Symbol_Buy(FixedMWPort(), szExpiry, 0);
	}
}

void LoadBoDWatchList()
{
	int nWatchMonth = GlobalValue("WATCHMONTH");

	if(!Is_Port_Loaded(FixedMWPort()))
	{
		char szCurrentDate[32] = "";
		int iCurrentDate = 0, iInstrType=0;
		Current_Date6(szCurrentDate);
		iCurrentDate = atoi(szCurrentDate);

		// Load Commodities: AU, AG, CU
		for(iInstrType = 0; iInstrType < 3; iInstrType++)
		{
			LoadCommodityWatchList(iInstrType, nWatchMonth, iCurrentDate);
		}

		LoadCSI300WatchList(iCurrentDate);
	}
}


// Get the Available Funds for an Investor
#ifndef _GET_AVAL_FUND_
#define _GET_AVAL_FUND_
double GetInvestorAvailFund(const char *pszSym)
{
        if (!pszSym)
                return 0;
        FlexAppRules::MarketInfo MktInfo(pszSym);
        //if(MktInfo.HasReceivedMarketData())
        //{
                return MktInfo.GetFlid(FLID_AVAIL, 0.0);
        //}
        //return 0;
}
#endif //#ifndef _GET_AVAL_FUND_

int GetGCD (int a, int b)
{
	for (;;)
	{
        if (a == 0) return b;
        b %= a;
        if (b == 0) return a;
        a %= b;
    }
}

int GetLCM (int a, int b)
{
	int temp = GetGCD(a, b);
    return temp ? (a / temp * b) : 0;
}

// UTILITY FUNCTION: Get the default tick size for different symbols
double	GetTickSizeForSymbol(const char* pszSymbol)
{
    if(!pszSymbol)
    {
    	return g_dEpsilon/10.0;
    }

    if(strstr(pszSymbol, "AU"))
    	return 0.01;
    else if(strstr(pszSymbol, "CU"))
    	return 10.0;
    else if(strstr(pszSymbol, "AG"))
    	return 1.0;
    else if(strstr(pszSymbol, "IF"))
    	return 0.2;

    return g_dEpsilon/10.0;
}

// UTILITY FUNCTION: Get the default contract size for different symbols
int	GetContractSizeForSymbol(const char* pszSymbol)
{
    if(!pszSymbol)
    {
    	return 0;
    }

    if(strstr(pszSymbol, "AU"))
    	return 1000;
    else if(strstr(pszSymbol, "CU"))
    	return 5;
    else if(strstr(pszSymbol, "AG"))
    	return 15;
    else if(strstr(pszSymbol, "IF"))
    	return 300;

    return 0;

}

// UTILITY FUNCTION: Get the default unit price for different symbols
double GetContractUnitPriceForSymbol(const char* pszSymbol)
{
    if(!pszSymbol)
    {
    	return 0;
    }

    return (double)(GetContractSizeForSymbol(pszSymbol)) ;
}
