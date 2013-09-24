#ifndef _BOSpdAdjustment_H_
#define _BOSpdAdjustment_H_

#include <RECommon.h>
#include <REHostExport.h>
#include <string.h>
#include "boost/unordered_map.hpp"
#include <SymConnection.h>

#ifndef _STRATCOMMON_FIXTAG_H_
#include "../Common/fixtag.h"
#endif

#ifndef _STRATCOMMON_LOGGER_H_
#include "../Common/logger.h"
#endif

namespace FlexStrategy
{
namespace BOSpdAdjustment
{
#define STR_LEN 32
#define STR_LEN_LONG 128

#define LEGID_MIN_BO	1
#define LEGID_BO_BQ		1
#define LEGID_BO_BH		2
#define	LEGID_BO_OQ		3
#define LEGID_BO_OH		4
#define LEGID_MAX_BO	4

static const double g_dEpsilon = 0.0001;
static const char cDelimiter = ';';

#define MODEID_MIN_BO		1
#define MODEID_INVALID		0
#define MODEID_ALWAYS		1
#define MODEID_CONDITIONAL	2
#define MODEID_AGGRESSIVE	3
#define MODEID_MAX_BO		3

/// Quoting Mode
enum E_STRAT_MODE {
	STRAT_MODE_INVALID		= MODEID_INVALID,
	STRAT_MODE_ALWAYS		= MODEID_ALWAYS,
	STRAT_MODE_CONDITIONAL	= MODEID_CONDITIONAL,
	STRAT_MODE_AGGRESSIVE	= MODEID_AGGRESSIVE
};

struct StratParams
{
	// Params
	long nStratId;
	char szStratPort[STR_LEN];
	E_STRAT_MODE eStratMode;
	char szSymbolQuote[STR_LEN]; //Quote Symbol
	char szSymbolHedge[STR_LEN]; //Hedge Symbol
	char szClientOrdIdBidQuote[STR_LEN];
	char szClientOrdIdOfferQuote[STR_LEN];
	char szClientOrdIdBidHedge[STR_LEN];
	char szClientOrdIdOfferHedge[STR_LEN];
	char szPortfolioBid[STR_LEN];
	char szPortfolioOffer[STR_LEN];
	double dBenchmarkBid;
	double dBenchmarkOffer;
	int nLotSizeQuote;
	int nLotSizeHedge;
	int nQtyPerOrder;
	int nPayUpTicks;
	int nMaxPosition;
	int nMaxRplLimit;
	double dMaxLoss;
	int nNumStages;
	int nSpreadInterval;
	int nStgAd; //adjustmnet
	int nAdInt;
	int nQuMul;
	int nHeMul;
	int nQuRa;
	int nHeRa;

	enOrderSide enSideBidQuote;
	enOrderSide enSideBidHedge;
	enOrderSide enSideOfferQuote;
	enOrderSide enSideOfferHedge;
	char szOMUser[STR_LEN];
	char szDest[STR_LEN];

	// Status
	bool bStratReady;
	bool bStratRunning;
	int	nQuoteRplCount;
	int nHedgeRplCount;
	double dStateBidBench;
	double dStateOfferBench;
	double dQuoteLastBotPrice;
	int nQuoteLastBotQty;
	double dQuoteLastSoldPrice;
	int nQuoteLastSoldQty;

	// Control Flags
	bool bBidQuoteEntry;
	bool bBidHedgeEntry;
	bool bOfferQuoteEntry;
	bool bOfferHedgeEntry;
	bool bBidQuoteRpld;
	bool bBidHedgeRpld;
	bool bOfferQuoteRpld;
	bool bOfferHedgeRpld;

	//MktData
	int nContraSizePrevBidQuote;
	double dContraPricePrevBidQuote;
	int nContraSizePrevOfferQuote;
	double dContraPricePrevOfferQuote;
	int nContraSizePrevBidHedge;
	double dContraPricePrevBidHedge;
	int nContraSizePrevOfferHedge;
	double dContraPricePrevOfferHedge;

	//@rafael store quote stage for hedge price
	int nQuoteBidStage;
	int nQuoteOfferStage;

	StratParams()
	{
		// Strat
		nStratId = 0;
		strcpy(szStratPort,"");
		eStratMode = STRAT_MODE_INVALID;
		strcpy(szSymbolQuote,"");
		strcpy(szSymbolHedge,"");
		strcpy(szClientOrdIdBidQuote,"");
		strcpy(szClientOrdIdOfferQuote,"");
		strcpy(szClientOrdIdBidHedge,"");
		strcpy(szClientOrdIdOfferHedge,"");
		strcpy(szPortfolioBid,"");
		strcpy(szPortfolioOffer,"");
		dBenchmarkBid = 0.0;
		dBenchmarkOffer = 0.0;
		nLotSizeQuote = 1;
		nLotSizeHedge = 1;
		nQtyPerOrder = 0;
		nPayUpTicks=0;
		nMaxPosition=0;
		nMaxRplLimit=0;
		dMaxLoss=0.0;
		nNumStages=1;
		nSpreadInterval=0;
		nStgAd = 1; //adjustmnet
		nAdInt = 0;
		nQuMul = 1;
		nHeMul = 1;
		nQuRa = 1;
		nHeRa = 1;
		enSideBidQuote = SIDE_INVALID;
		enSideBidHedge = SIDE_INVALID;
		enSideOfferQuote = SIDE_INVALID;
		enSideOfferHedge = SIDE_INVALID;
		strcpy(szOMUser,"");
		strcpy(szDest,"");

		//Status
		bStratReady = false;
		bStratRunning = false;
		nQuoteRplCount=0;
		nHedgeRplCount=0;
		dStateBidBench = 0.0;
		dStateOfferBench = 0.0;
		dQuoteLastBotPrice = 0.0;
		nQuoteLastBotQty = 0;
		dQuoteLastSoldPrice = 0.0;
		nQuoteLastSoldQty = 0;

		// Control Flags
		bBidQuoteEntry = false;
		bBidHedgeEntry = false;
		bOfferQuoteEntry = false;
		bOfferHedgeEntry = false;
		bBidQuoteRpld = false;
		bBidHedgeRpld = false;
		bOfferQuoteRpld = false;
		bOfferHedgeRpld = false;


		//MktData
		nContraSizePrevBidQuote=0;
		dContraPricePrevBidQuote=0.0;
		nContraSizePrevOfferQuote=0;
		dContraPricePrevOfferQuote=0.0;
		nContraSizePrevBidHedge = 0;
		dContraPricePrevBidHedge = 0.0;
		nContraSizePrevOfferHedge = 0;;
		dContraPricePrevOfferHedge = 0.0;

		//@rafael
		nQuoteBidStage = 0;
		nQuoteOfferStage = 0;

	}
};


typedef boost::unordered_map<long,StratParams*> STRATMAP;
typedef boost::unordered_map<long,StratParams*>::iterator STRATMAPITER;

typedef boost::unordered_map<std::string,long> STRATIDMAP;
typedef boost::unordered_map<std::string,long>::iterator STRATIDMAPITER;

typedef boost::unordered_map<std::string,STREET_ORDER> SHFEORDRPLMAP;
typedef boost::unordered_map<std::string,STREET_ORDER>::iterator SHFEORDERRPLMAPITER;


typedef std::multimap<double, STREET_ORDER> SORTED_ORDERS_MULTIMAP;
typedef std::multimap<double, STREET_ORDER>::iterator SORTED_ORDERS_MULTIMAP_ITER;
typedef std::multimap<double, STREET_ORDER>::reverse_iterator SORTED_ORDERS_MULTIMAP_REVITER;

class BOSpdAdjustment : public StrategyBase
{
public:

	BOSpdAdjustment();
	virtual ~BOSpdAdjustment();

	//Market Data event implementation
	int OnMarketData(CLIENT_ORDER& ClientOrder, MTICK& MTick);
	int OnMarketData(MTICK& MTick);
	int OnLoad(const char* szConfigFile = NULL);

	// Connection call backs
	int OnClientConnect(const char* szDest);
	int OnClientDisconnect(const char* szDest);

	int OnStreetConnect(const char* szDest);
	int OnStreetDisconnect(const char* szDest);

	int OnConfigUpdate(const char* szConfigBuf);
	int OnClientCommand(STRAT_COMMAND &pCommand);

	int OnEndOfRecovery();

	int OnTimer(EVENT_DATA& eventData);

	//Following are on Client events for his particular orders
	int OnClientOrdNew(CLIENT_ORDER& ClientOrd);
	int OnClientOrdRpl(CLIENT_ORDER& ClientOrd);
	int OnClientOrdCancel(CLIENT_ORDER& ClientOrd);
	int OnClientSendAck(CLIENT_EXEC& ClientOrd);
	int OnClientSendOut(CLIENT_EXEC& ClientOrd);
	int OnClientSendReject(CLIENT_EXEC& ClientExec);
	int OnClientSendFills(CLIENT_EXEC& ClientExec);
	int OnClientOrdValidNew(CLIENT_ORDER& ClientOrd);
	int OnClientOrdValidRpl(CLIENT_ORDER& ClientOrd);
	int OnClientOrdValidCancel(CLIENT_ORDER& ClientOrd);
	int OnClientSendPendingRpl(OMRULESEXPORT::CLIENT_EXEC& ClientExec);

	// Street Orders
	int OnStreetOrdNew(STREET_ORDER& StreetOrd, CLIENT_ORDER& ParentOrder);
	int OnStreetOrdRpl(STREET_ORDER& StreetOrd, CLIENT_ORDER& ParentOrder);
	int OnStreetOrdCancel(STREET_ORDER& StreetOrd, CLIENT_ORDER& ParentOrder);
	int OnStreetOrdAck(STREET_ORDER& StreetOrd);
	int OnStreetOrdRej(STREET_ORDER& StreetOrd, CLIENT_ORDER& ParentOrder);
	int OnStreetOrdOut(STREET_ORDER& StreetOrd);
	int OnStreetExec(STREET_EXEC& StreetExec, CLIENT_ORDER& ParentOrder);
	int OnStreetOrdValidNew(STREET_ORDER& StreetOrd, CLIENT_ORDER& ParentOrder);
	int OnStreetOrdValidRpl(STREET_ORDER& StreetOrd, CLIENT_ORDER& ParentOrder);
	int OnStreetOrdValidCancel(STREET_ORDER& StreetOrd, CLIENT_ORDER& ParentOrder);
	int OnStreetStatusReport(STREET_EXEC& StreetExec, CLIENT_ORDER& ParentOrder);
	int OnStreetNewReportStat(STREET_EXEC& StreetExec, CLIENT_ORDER& ParentOrder);
	int OnStreetDiscardOrdCancel(STREET_ORDER& StreetOrd, CLIENT_ORDER& ParentOrder);

	//Custom
	StratParams* GetActiveStratParamsByClientOrderId(const char* pszClientOrderId);
	// MarketData
	bool 	IsValidMarketTick(MTICK& mtick);
	bool	IsMarketSpreadChanged(StratParams* pStratParams, MTICK& mtick_quote, MTICK& mtick_hedge, bool bBidSpread);
	bool	IsSpreadHedgeChanged(StratParams* pStratParams, MTICK& mtick_hedge, bool bBidSpread);
	bool	IsSpreadQuoteChanged(StratParams* pStratParams, MTICK& mtick_quote, bool bBidSpread);
	bool	IsSpreadBenchSatisfied(StratParams* pStratParams, MTICK& mtick_quote, MTICK& mtick_hedge, bool bBidSpread);
	// Strat - Params
	bool	UpdateStageBenchmarks(StratParams* pStratParams);
	bool 	UpdateHedgeStageBenchmarks(StratParams* pStratParams); //@rafael
	double	GetBiddingBenchmark(StratParams* pStratParams);
	double	GetOfferingBenchmark(StratParams* pStratParams);
	double	GetHedgeBiddingBenchmark(StratParams* pStratParams);  //@rafael
	double	GetHedgeOfferingBenchmark(StratParams* pStratParams); //@rafael
	int		CalculateNetStratExecs(StratParams* pStratParams);
	// Strat - Client
	int		ValidateFIXTags(ORDER& order);
	int		AddStratParamsToMap(ORDER& order,STRATMAP& pStMap);
	int		GetQuoteClientOrder(CLIENT_ORDER& QuoteOrder, StratParams* pStratParams, bool bBidSpread);
	int		GetHedgeClientOrder(CLIENT_ORDER& HedgeOrder, StratParams* pStratParams, bool bBidSpread);
	bool	IsHedgeAdjustComplete (StratParams* pStratParams, int nNetStratExecs);
	int		HandleHedgeAdjustWorkingSize (StratParams* pStratParams, int nNetStratExecs, double dDriverLastPrice, int nDriverLastQty, int nDriverLastLegId);
	int		HandleQuoteUponMarketData (StratParams* pStratParams, MTICK& mtick_quote, MTICK& mtick_hedge, bool bBidSpread);
	int		HandleQuoteUponInvalidSpread(StratParams* pStratParams, bool bBidSpread);
	int		HandleHedgeOnQuoteExecs(STREET_EXEC& StreetExec, CLIENT_ORDER& ParentOrder);
	int		HandleManualOrderUponCommand(long nStratId, const char* szSymbol, double dPrice, int nOrdQty, const char* szSide, int nLegId, const char* szInstr); //0903

	double	GetStratQuotePrice (double dBenchmark, MTICK& mtick_quote, MTICK& mtick_hedge, bool bBidSpread,int nQuMul ,int nHeMul);
	double	GetStratHedgePrice (StratParams* pStratParams, double dDriverLastPrice, bool bBidHedge); //@rafael
	int		CalculateNewQuoteQuantity(StratParams* pStratParams, bool bBidSpread);
	int		CalculateQuoteQtyLimit(MTICK& mtick_quote, bool bBidSpread);
	int		HandleSHFEReplace(STREET_ORDER& Order, int nOrderQty, double dOrderPrice);
	int		OrderPriceCrossCheck(STREET_ORDER& NewStreetOrder); //0903
	int 	ExecsRiskLimitControl(STREET_EXEC& StreetExec, CLIENT_ORDER& ParentOrder);
	int		UpdateStratQuoteCxlRplCount(STREET_ORDER& StreetOrder, CLIENT_ORDER& ParentOrder);//0903
	int		ReplaceRiskLimitControl(CLIENT_ORDER& ClientOrder);
	int 	UpdateStratParamWithClientRpld(StratParams* pStratParams, StratParams* pStratParamsRpl);
	int		PublishStratStatus(StratParams* pStratParams);
	int		PublishStratLoss(StratParams* pStratParams,  MTICK& mtick_quote, MTICK& mtick_hedge, double dUnitPriceQuote,double dUnitPriceHedge, double dNetLossQuote, double dNetLossHedge, CLIENT_ORDER& BidQuoteOrder, CLIENT_ORDER& BidHedgeOrder, CLIENT_ORDER& OfferQuoteOrder, CLIENT_ORDER& OfferHedgeOrder);

	// Utils
	int		SendOutStreetOrder(CLIENT_ORDER& ClientOrder, const char* pszDestination, int nOrderQty, double dOrderPrice);
	int		StopStratOnRiskLimitBreached(StratParams* pStratParams);
	int		CancelExcessOrderForClientOrder(const char* pszClientOrderId, int nNetStratExecs);
	int 	CancelAllOrdersForClientOrder(const char* pszClientOrderId);
	int 	CancelAllStreetOrders(const StratParams *pStratParams);
	bool	CanReplace(STREET_ORDER& StreetOrder);
	const char* StateToStr(enOrderState e);
	const char* StratModeToStr(E_STRAT_MODE e);
	const char* ErrorCodeToStr(enErrorCode e);
	double	GetTickSizeForSymbol(const char* pszSymbol);
	int		GetContractSizeForSymbol(const char* pszSymbol);
	double	GetContractUnitPriceForSymbol(const char* pszSymbol);

private:
	void TestLevel2(std::string symbol);
	void PrintStratOrderMap(STRATMAP& mpStratMap, const long nStratId);
	void PrintStratOrderMapRpl(STRATMAP& mpStratMapRpl);
	void PrintStratOrderMapCxl(STRATMAP& mpStratMapCxl);
	void PrintStreetExec(STREET_EXEC& StreetExec, CLIENT_ORDER& parentOrder);
	void PrintSHFEOrdRplMap(SHFEORDRPLMAP& gmSHFEOrdRplMap);
	void PrintSortedOrdersMultimap(SORTED_ORDERS_MULTIMAP& multimap);
	bool SymConnectionInit();

private:
	static char *m_sFixTagStrategyName;
	static std::string m_sPropertiesFileName;
	static std::string m_sFixDestination;

	static int m_sCount;
	static int m_sStreetOrderSize;
};

// BidOffer Spread
static const unsigned int	C_BOSPD_NUM_OF_LEGS_INDEX	= 3; // Num of legs = 4: [0,1,2,3]
static const unsigned int	C_BOSPD_NUM_OF_LEGS		= 4; // Num of legs = 4


extern STRATIDMAP gmStratIdMap;
extern STRATMAP gmStratMap, gmStratMapRpl;
extern STRATMAP gmStratMapCxl;

extern SHFEORDRPLMAP gmSHFEOrdRplMap;

}; // namespace BOSpdAdjustment
}; // namespace FlexStrategy

#endif //_BOSpdAdjustment_H_
