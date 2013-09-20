#include "./BidOfferSpread.h"
#include <sstream>
#include <map>
#include <sys/time.h>

using namespace FlexStrategy::BidOfferSpread;

char *BidOfferSpread::m_sFixTagStrategyName = "BOSP";

// For Strat
STRATIDMAP FlexStrategy::BidOfferSpread::gmStratIdMap;
STRATMAP FlexStrategy::BidOfferSpread::gmStratMap, FlexStrategy::BidOfferSpread::gmStratMapRpl;
STRATMAP FlexStrategy::BidOfferSpread::gmStratMapCxl;

// For SHFE Rpl
SHFEORDRPLMAP FlexStrategy::BidOfferSpread::gmSHFEOrdRplMap;

// For SymCommand
static CSymConnection *g_SymConnection = NULL;
static bool gb_SymConnected = false;

// For OR Reject Message
const int FIX_TAG_OR_MSG = 8101;

BidOfferSpread::BidOfferSpread() :
	StrategyBase(m_sFixTagStrategyName, LOG_ERROR)
{
	CINFO << "----------------------------------------------------------------------" << std::endl;
	CINFO << " In BidOfferSpread CTOR strat name = " << GetStratName() << std::endl;
	CINFO << " Build Date [" << BUILD_DATE << "]" << std::endl;
	CINFO << " Build Time [" << BUILD_TIME << "]" << std::endl;
	CINFO << " Build Version [" << BUILD_VERSION << "]" << std::endl;

	switch (STRAT_DEBUG_LEVEL)
	{
		case 0:		CINFO << " Logging Level: Info " << std::endl;		break;
		case 1:		CINFO << " Logging Level: Verbose " << std::endl;	break;
		default:	CINFO << " Logging Level: Debug " << std::endl;		break;
	}

	if(!SymConnectionInit())
	{
		CWARN << "Failed to initialize SymConnection!" << std::endl;
		gb_SymConnected = false;
	}
	else
	{
		CINFO << "Successfully initialized SymConnection!" << std::endl;
		gb_SymConnected = true;
	}

	CINFO << "----------------------------------------------------------------------" << std::endl;
}

BidOfferSpread::~BidOfferSpread()
{
	CINFO << __PRETTY_FUNCTION__<< std::endl;
}

int BidOfferSpread::OnLoad(const char* szConfigFile)
{
	CINFO << __PRETTY_FUNCTION__<< std::endl;
	SetSignalEnabled(false);
	return SUCCESS;
}

int BidOfferSpread::OnEndOfRecovery(){return SUCCESS;}

int BidOfferSpread::OnMarketData(CLIENT_ORDER& order, MTICK& mtick)
{
	_STRAT_LOG_VERB_(CINFO << "\n \n=====On Market Data with order ID [" << order.GetHandle()
							<< "] Symbol [" << mtick.GetSymbol()
							<< "]====="  << std::endl);

	// Validation : Market Tick
	if (!IsValidMarketTick(mtick))
		return FAILURE;

	// Validate : Order Entry
	if(order.GetUnOrdSize() < 1)
	{
		_STRAT_LOG_VERB_(CWARN << "Order ID [" << order.GetHandle()
				<< "No Quantity! UnordSize [" << order.GetUnOrdSize() << "]"
				<< std::endl);
		return FAILURE;
	}

	// STEP 1 : Get active strat param by client order id and decide if react to market data
	StratParams *pStratParams = NULL;
	pStratParams = GetActiveStratParamsByClientOrderId(order.GetHandle());
	if (!pStratParams)
	{
		_STRAT_LOG_VERB_(CWARN << "No active StratParam with ClientOrderId [" << order.GetHandle() << "]" << std::endl);
		return FAILURE;
	}


	// STEP 2 : Validate if Strat is ready and running
	if(! ((pStratParams->bStratReady)&&(pStratParams->bStratRunning)))
	{
		_STRAT_LOG_VERB_(CWARN << "StratID [" << pStratParams->nStratId
								<< "] is stopped: ReadyFlag? [" << pStratParams->bStratReady
								<< "] isRuning? [" << pStratParams->bStratRunning
								<< "]"
								<< std::endl);
		return FAILURE;
	}

	
	// STEP 3 : Get the relevant market data fields and save in mtick_quote and mtick_hedge
	MTICK mtick_quote, mtick_hedge;
	if(!strcmp(order.GetSymbol(),pStratParams->szSymbolQuote))
	{
		mtick_quote = mtick;
		MarketDataGetSymbolInfo(pStratParams->szSymbolHedge, mtick_hedge);

		if (pStratParams->eStratMode == STRAT_MODE_ALWAYS)
		{
			// STRAT_MODE_ALWAYS: Disregard Quote Contract Market Ticks
			_STRAT_LOG_VERB_(CWARN << "Strat ID [" <<  pStratParams->nStratId
						<< "] Quote Symbol [" << pStratParams->szSymbolQuote
						<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
						<< "] MTick Symbol [" << order.GetSymbol()
						<< "] : Disregard Quoting Contract Market Ticks!"<< std::endl);
			return SUCCESS;
		}
	}
	else if(!strcmp(order.GetSymbol(),pStratParams->szSymbolHedge))
	{
		mtick_hedge = mtick;
		MarketDataGetSymbolInfo(pStratParams->szSymbolQuote, mtick_quote);
	}
	else
	{
		_STRAT_LOG_DBUG_(CERR << "Failed to get relevant market data fields!"<< std::endl);
		return FAILURE;
	}

	if (!(IsValidMarketTick(mtick_quote) && IsValidMarketTick(mtick_hedge)))
		return FAILURE;


	// STEP 4 : Check if relevant market data fields have been updated
	bool bIsBidQuoteChange = IsSpreadQuoteChanged(pStratParams, mtick_quote, true);
	bool bIsOfferQuoteChange = IsSpreadQuoteChanged(pStratParams, mtick_quote, false);

	bool bIsBidHedgeChange = IsSpreadHedgeChanged(pStratParams, mtick_hedge, true);
	bool bIsOfferHedgeChange = IsSpreadHedgeChanged(pStratParams, mtick_hedge, false);

	if (!(bIsBidQuoteChange || bIsOfferQuoteChange || bIsBidHedgeChange || bIsOfferHedgeChange))
	{
		_STRAT_LOG_VERB_(CWARN << "Market data not updated!"<< std::endl);
		return FAILURE;
	}


	// STEP 5: Calculate NetStratExecs
	int nNetStratExecs = 0;
	nNetStratExecs = CalculateNetStratExecs(pStratParams);

	bool bHedgeAdjusted = IsHedgeAdjustComplete (pStratParams, nNetStratExecs);

	// Handle Hedge
	if (!bHedgeAdjusted)
	{
		_STRAT_LOG_DBUG_(CDEBUG << "StratID [" << pStratParams->nStratId
								<< "] Hedge Adjusted? [" << bHedgeAdjusted
								<< "] - Proceed to Adjust Hedge Working Size!"
								<< "]"
								<< std::endl);

		if (HandleHedgeAdjustWorkingSize (pStratParams, nNetStratExecs, 0.0, 0, 0) == SUCCESS)
		{
			_STRAT_LOG_VERB_(CWARN << "StratID [" << pStratParams->nStratId
									<< "] : Successful in attempt to adjust Hedge working size."
									<< std::endl);
		}
		else
		{
			_STRAT_LOG_VERB_(CERR << "StratID [" << pStratParams->nStratId
									<< "] : FAIL in attempt to adjust Hedge working size!"
									<< std::endl);
		}
	}

	// Handle Quote
	if (pStratParams->eStratMode == STRAT_MODE_ALWAYS)
	{
		// Send/replace Quote orders as long as Hedge price changes
		if (bIsBidHedgeChange)
		{
			HandleQuoteUponMarketData (pStratParams, mtick_quote, mtick_hedge, true);
		}

		if (bIsOfferHedgeChange)
		{
			HandleQuoteUponMarketData (pStratParams, mtick_quote, mtick_hedge, false);
		}
		return SUCCESS;
	}
	else if (pStratParams->eStratMode == STRAT_MODE_CONDITIONAL)
	{
		// Mkt Bid Spread = Bid_Quote - Bid_Hedge
		// Mkt Offer Spread = Offer_Quote - Offer_Hedge
		bool bIsBidBenchSatisfied = IsSpreadBenchSatisfied(pStratParams, mtick_quote, mtick_hedge, true);

		if (bIsBidBenchSatisfied)
		{
			if(bIsBidHedgeChange)
			{
				// Send new bid quote or replace existing bid quote
				_STRAT_LOG_DBUG_(CDEBUG << "StratID [" << pStratParams->nStratId
										<< "] QuoteMode [ " << StratModeToStr(pStratParams->eStratMode)
										<< "] IsBidBenchSatisfied? [" << bIsBidBenchSatisfied
										<< "] IsBidHedgeChange? [" << bIsBidHedgeChange
										<< "] - Proceed to Handle Bid Quote New/Rpl!"
										<< std::endl);

				HandleQuoteUponMarketData (pStratParams, mtick_quote, mtick_hedge, true);
			}
		}
		else
		{
			// Cancel existing bid quote
			_STRAT_LOG_DBUG_(CDEBUG << "StratID [" << pStratParams->nStratId
									<< "] QuoteMode [ " << StratModeToStr(pStratParams->eStratMode)
									<< "] IsBidBenchSatisfied? [" << bIsBidBenchSatisfied
									<< "] - Proceed to Handle Bid Quote Cxl!"
									<< std::endl);

			HandleQuoteUponInvalidSpread(pStratParams, true);
		}

		bool bIsOfferBenchSatisfied = IsSpreadBenchSatisfied(pStratParams, mtick_quote, mtick_hedge, false);

		if(bIsOfferBenchSatisfied)
		{
			if(bIsOfferHedgeChange)
			{
				// Send new offer quote or replace existing offer quote
				_STRAT_LOG_DBUG_(CDEBUG << "StratID [" << pStratParams->nStratId
										<< "] QuoteMode [ " << StratModeToStr(pStratParams->eStratMode)
										<< "] IsOfferBenchSatisfied? [" << bIsOfferBenchSatisfied
										<< "] IsOfferHedgeChange? [" << bIsOfferHedgeChange
										<< "] - Proceed to Handle Offer Quote New/Rpl!"
										<< std::endl);

				HandleQuoteUponMarketData (pStratParams, mtick_quote, mtick_hedge, false);
			}
		}
		else
		{
			// Cancel existing offer quote
			_STRAT_LOG_DBUG_(CDEBUG << "StratID [" << pStratParams->nStratId
									<< "] QuoteMode [ " << StratModeToStr(pStratParams->eStratMode)
									<< "] IsOfferBenchSatisfied? [" << bIsOfferBenchSatisfied
									<< "] - Proceed to Handle Offer Quote Cxl!"
									<< std::endl);

			HandleQuoteUponInvalidSpread(pStratParams, false);
		}

		return SUCCESS;
	}
	else if (pStratParams->eStratMode == STRAT_MODE_AGGRESSIVE)
	{
		// Mkt Bid Spread = Offer_Quote - Bid_Hedge
		// Mkt Offer Spread = Bid_Quote - Offer_Hedge

		bool bIsBidBenchSatisfied = IsSpreadBenchSatisfied(pStratParams, mtick_quote, mtick_hedge, true);

		if (bIsBidBenchSatisfied)
		{
			if(bIsBidHedgeChange || bIsBidQuoteChange)
			{
				// Send new bid quote or replace existing bid quote
				_STRAT_LOG_DBUG_(CDEBUG << "StratID [" << pStratParams->nStratId
										<< "] QuoteMode [ " << StratModeToStr(pStratParams->eStratMode)
										<< "] IsBidBenchSatisfied? [" << bIsBidBenchSatisfied
										<< "] IsBidHedgeChange? [" << bIsBidHedgeChange
										<< "] - Proceed to Handle Bid Quote New/Rpl!"
										<< std::endl);

				HandleQuoteUponMarketData (pStratParams, mtick_quote, mtick_hedge, true);
			}
		}
		else
		{
			// Cancel existing bid quote
			_STRAT_LOG_DBUG_(CDEBUG << "StratID [" << pStratParams->nStratId
									<< "] QuoteMode [ " << StratModeToStr(pStratParams->eStratMode)
									<< "] IsBidBenchSatisfied? [" << bIsBidBenchSatisfied
									<< "] - Proceed to Handle Bid Quote Cxl!"
									<< std::endl);

			HandleQuoteUponInvalidSpread(pStratParams, true);
		}

		bool bIsOfferBenchSatisfied = IsSpreadBenchSatisfied(pStratParams, mtick_quote, mtick_hedge, false);

		if(bIsOfferBenchSatisfied)
		{
			if(bIsOfferHedgeChange || bIsOfferQuoteChange)
			{
				// Send new offer quote or replace existing offer quote
				_STRAT_LOG_DBUG_(CDEBUG << "StratID [" << pStratParams->nStratId
										<< "] IsOfferBenchSatisfied? [" << bIsOfferBenchSatisfied
										<< "] IsOfferHedgeChange? [" << bIsOfferHedgeChange
										<< "] - Proceed to Handle Offer Quote New/Rpl!"
										<< std::endl);

				HandleQuoteUponMarketData (pStratParams, mtick_quote, mtick_hedge, false);
			}
		}
		else
		{
			// Cancel existing offer quote
			_STRAT_LOG_DBUG_(CDEBUG << "StratID [" << pStratParams->nStratId
									<< "] IsOfferBenchSatisfied? [" << bIsOfferBenchSatisfied
									<< "] - Proceed to Handle Offer Quote Cxl!"
									<< std::endl);

			HandleQuoteUponInvalidSpread(pStratParams, false);
		}

		return SUCCESS;
	}

	return SUCCESS;
}

int BidOfferSpread::OnMarketData(MTICK& MTick){return SUCCESS;}

// Connection call backs
int BidOfferSpread::OnClientConnect(const char* szDest)
{
	CINFO << __PRETTY_FUNCTION__ << std::endl;
	return SUCCESS;
}

int BidOfferSpread::OnClientDisconnect(const char* szDest)
{
	CINFO << __PRETTY_FUNCTION__ << std::endl;
	return SUCCESS;
}

int BidOfferSpread::OnStreetConnect(const char* szDest)
{
	CINFO << __PRETTY_FUNCTION__ << std::endl;
	return SUCCESS;
}

int BidOfferSpread::OnStreetDisconnect(const char* szDest)
{
	CINFO << __PRETTY_FUNCTION__ << std::endl;
	return SUCCESS;
}

int BidOfferSpread::OnConfigUpdate(const char* szConfigBuf)
{
	CINFO << __PRETTY_FUNCTION__ << " Got string buf = " << szConfigBuf << std::endl;
	return SUCCESS;
}

int BidOfferSpread::OnClientCommand(STRAT_COMMAND &pCommand)
{
	static char* CMD_STRAT="STRAT";
	static char* CMD_STRATID="STRATID";
	static char* CMD_SYM ="SYMBOL";
	static char* CMD_PRICE ="PRICE";
	static char* CMD_QTY ="ORDQTY";
	static char* CMD_SIDE ="SIDE";
	static char* CMD_LEGID ="LEGID";
	static char* CMD_INSTR ="INSTRUCTION";

	char szStrat[STR_LEN]="";
	pCommand.GetParam(CMD_STRAT, szStrat,STR_LEN);

	char szStratId[STR_LEN]="";
	pCommand.GetParam(CMD_STRATID, szStratId, STR_LEN);
	long nStratId = 0;
	nStratId = atol (szStratId);

	char szSymbol[STR_LEN]="";
	pCommand.GetParam(CMD_SYM, szSymbol, STR_LEN);

	char szPrice[STR_LEN]="";
	pCommand.GetParam(CMD_PRICE, szPrice, STR_LEN);
	double dPrice = 0.0;
	dPrice = atof (szPrice);

	char szOrdQty[STR_LEN]="";
	pCommand.GetParam(CMD_QTY, szOrdQty, STR_LEN);
	int nOrdQty = 0;
	nOrdQty = atoi (szOrdQty);

	char szSide[STR_LEN]="";
	pCommand.GetParam(CMD_SIDE, szSide, STR_LEN);

	char szLegId[STR_LEN]="";
	pCommand.GetParam(CMD_LEGID, szLegId, STR_LEN);
	int nLegId = 0;
	nLegId = atoi (szLegId);

	char szInstr[STR_LEN]="";
	pCommand.GetParam(CMD_INSTR, szInstr, STR_LEN);

	CINFO << "Command Received: " << CMD_STRAT << " [" << szStrat
			<< "] " << CMD_STRATID << " [" << nStratId
			<< "] " << CMD_SYM << " [" << szSymbol
			<< "] " << CMD_PRICE << " [" << dPrice
			<< "] " << CMD_QTY << " [" << nOrdQty
			<< "] " << CMD_SIDE << " [" << szSide
			<< "] " << CMD_LEGID << " [" << nLegId
			<< "] " << CMD_INSTR << " [" << szInstr
			<< "]!"
			<< std::endl;

	return HandleManualOrderUponCommand(nStratId, szSymbol, dPrice, nOrdQty, szSide, nLegId, szInstr);
}

// Client Order Callbacks
int BidOfferSpread::OnClientOrdValidNew(CLIENT_ORDER& order)
{
	_STRAT_LOG_DBUG_(CDEBUG << "Checking SymConnection -> gb_SymConnected  [" << gb_SymConnected
								<< "]"
								<< std::endl);
	if (!gb_SymConnected)
	{
		CWARN << "No Connection to Sym -> gb_SymConnected [" << gb_SymConnected
					<< "] : Re-initializing SymConnection!" << std::endl;

		if(!SymConnectionInit())
		{
			CWARN << "Failed to re-initialize SymConnection!" << std::endl;
			gb_SymConnected = false;
		}
		else
		{
			CWARN << "Successfully re-initialized SymConnection!" << std::endl;
			gb_SymConnected = true;
		}
	}

	return BidOfferSpread::ValidateFIXTags(order);
}

int BidOfferSpread::OnClientOrdNew(CLIENT_ORDER& order)
{
	long nStratId = atol(order.GetFixTag(FIX_TAG_STRATID_BO));
	std::string strOrderId(order.GetHandle());

	// Step 1: Insert the StratID into gmStratIdMap
	_STRAT_LOG_DBUG_(CDEBUG << "Before Client New: Inserting StratId [" << nStratId
							<<"] and OrderId [" <<strOrderId
							<<"] into gmStratIdMap!"
							<< std::endl);

	gmStratIdMap.insert(pair<std::string,long>(strOrderId,nStratId));


	// Step 2: Insert the order into gmStratMap
	if (AddStratParamsToMap(order, gmStratMap) != SUCCESS)
	{
		_STRAT_LOG_VERB_(CERR << "Debug: Fail to Add New orders to Strat Map - StratId ["<< nStratId
								<< "] Symbol [" <<order.GetSymbol()
								<< "] Size ["<< order.GetSize()
								<< "] Handle [" << order.GetHandle()
								<< "] FixTags [" <<order.GetFixTags()
								<< "]"
								<< std::endl);
		return FAILURE;
	}


	// Step 3: Logging for NEW
	CINFO << "CLIENT NEW: StratId [" << nStratId
			<<"] Symbol [" <<order.GetSymbol()
			<<"] Size [" <<order.GetSize()
			<<"] Handle [" << order.GetHandle()
			<<"] FixTags [" <<order.GetFixTags()
			<<"] " << std::endl;

	return SUCCESS;
}

int BidOfferSpread::OnClientOrdValidRpl(CLIENT_ORDER& order)
{
    _STRAT_LOG_DBUG_(CDEBUG << "Debug: Validate Client RPL StratID [" << order.GetFixTag(FIX_TAG_STRATID_BO) << "]" <<std::endl);

	return BidOfferSpread::ValidateFIXTags(order);
}

int BidOfferSpread::OnClientOrdRpl(CLIENT_ORDER& order)
{
	_STRAT_LOG_DBUG_(CDEBUG << "Start Client RPL - StratID [" << order.GetFixTag(FIX_TAG_STRATID_BO)
			<< "] Client OrderID [" << order.GetHandle()
        	<< "] Client RefOrdId [" << order.GetRefOrdId()
        	<< "] Client NewOrderId [" << order.GetNewOrderId()
        	<< "] Client CommonOrderId [" << order.GetCommonOrderId()
			<< "] Client GetPrimOrdId [" << order.GetPrimOrdId()
			<< "] Client GetOrigOrderId [" << order.GetOrigOrderId()
			<< "] Client Symbol [" << order.GetSymbol()
        	<< "]"
        	<<std::endl);


	// Step 1: Insert client order into RplMap
	_STRAT_LOG_VERB_(CWARN << "Adding Client OrderID [" << order.GetHandle()
			<< "] Client GetRefOrdId [" << order.GetRefOrdId()
        	<< "] Client Symbol [" << order.GetSymbol()
        	<< "] to gmStratMapRpl"
        	<<std::endl);
	AddStratParamsToMap(order,gmStratMapRpl);


	// Step 2: Find StratParams in gmStratMap and gmStratMapRpl
	// Get pStratParams and pStratParamsRpl for further processing
	long nStratId = atol(order.GetFixTag(FIX_TAG_STRATID_BO));
	STRATMAPITER mStIterRpl = gmStratMapRpl.find(nStratId);
	STRATMAPITER mStIter = gmStratMap.find(nStratId);

	if((mStIter == gmStratMap.end()) ||(mStIterRpl == gmStratMapRpl.end()))
	{
        _STRAT_LOG_VERB_(CERR << "StratID [" << nStratId << "] not found in gmStratMap or gmStratMapRpl" << std::endl);
		return FAILURE;
	}

	StratParams *pStratParamsRpl = (StratParams*) mStIterRpl->second;
	StratParams *pStratParams = (StratParams*) mStIter->second;

	if(!pStratParams || !pStratParamsRpl)
	{
        _STRAT_LOG_VERB_(CERR << "Invalid pointer pStratParams or pStratParamsRpl " << std::endl);
		return FAILURE;
	}


	// Step 3 : Handle client replace
	// - If strategy is still running, stop strategy and cancel all street orders.
	// - Otherwise just replace client orders
	if (pStratParams->bStratRunning == false)
	{
		_STRAT_LOG_VERB_(CWARN << "StratID [" << pStratParams->nStratId
				<< "] Running Flag [" << pStratParams->bStratRunning
				<< "] : Already Stopped. Will not send street cancel again."
				<< std::endl);
	}
	else
	{
		_STRAT_LOG_VERB_(CWARN << "StratID [" << pStratParams->nStratId
				<< "] Running Flag [" << pStratParams->bStratRunning
				<< "] : Now stop strategy and send street cancels."
				<< std::endl);

		// Step 3-1: Stop the current working Strat because client has sent a replace.
		// Therefore, until replace is successful just stop this Strat.
		pStratParams->bStratRunning = false;

		// Step 3-2 : Cancel all street orders of the strat
		_STRAT_LOG_VERB_(CWARN << "StratID [" << pStratParams->nStratId
								<< "] : Sending Cancels!"
								<< std::endl);

		if (CancelAllStreetOrders(pStratParams) != SUCCESS)
		{
			bool bRunning = (bool)atoi(order.GetFixTag(FIX_TAG_RUNNING_BO));

			if (bRunning)
			{
				_STRAT_LOG_VERB_(CWARN << "StratID [" << nStratId
						 << "] RPL for Client OrderID [" << order.GetHandle()
						 << "] with RunningFlag [" << order.GetFixTag(FIX_TAG_RUNNING_BO)
						 << "] FAILED!!! -> CXL-REQ could not be sent out on all the active street orders"
						 << std::endl);
			}
			else
			{
				_STRAT_LOG_VERB_(CERR << "StratID [" << nStratId
						<< "] RPL for Client OrderID [" << order.GetHandle()
						<< "] with RunningFlag [" << order.GetFixTag(FIX_TAG_RUNNING_BO)
						<< "] FAILED!!! -> CXL-REQ could not be sent out on all the active street orders"
						<< std::endl);
				return FAILURE;
			}
		}
	}

	_STRAT_LOG_DBUG_(PrintStratOrderMap(gmStratMap, nStratId));
	_STRAT_LOG_VERB_(PrintStratOrderMapRpl(gmStratMapRpl));

	// Step 4 : Insert NewId into StratIdMap
	std::string strOrderId(order.GetNewOrderId());
	gmStratIdMap.insert(pair<std::string, long>(strOrderId, nStratId));


	// Step 5: Logging for RPL
	CINFO << "StratID [" << nStratId
			 << "] RPL : Client OrderID [" << order.GetHandle()
			 << "] Symbol [" << order.GetSymbol()
			 << "] Size [" << order.GetSize()
			 << "] FixTags [" << order.GetFixTags()
			 << "]" << std::endl;

	return SUCCESS;
}

int BidOfferSpread::OnClientOrdValidCancel(CLIENT_ORDER& order){return SUCCESS;}

int BidOfferSpread::OnClientOrdCancel(CLIENT_ORDER& order)
{
	long nStratId = atol(order.GetFixTag(FIX_TAG_STRATID_BO));
	CINFO << "CXL: StratID [" << nStratId
			<<"] Symbol [" << order.GetSymbol()
			<<"] Size [" << order.GetSize()
			<<"] Handle [" << order.GetHandle()
			<<"] FixTags [" << order.GetFixTags()
			<<"]" << std::endl;
	return SUCCESS;
}

int BidOfferSpread::OnClientSendFills(CLIENT_EXEC& clientExec){return SUCCESS;}

int BidOfferSpread::OnClientSendAck(CLIENT_EXEC& ClientExec)
{
	CLIENT_ORDER ClientOrder;
	ClientOrder = ClientExec.GetClientOrder();
	enOrderState eOrdState = ClientExec.GetReportStatus();

	_STRAT_LOG_DBUG_(CDEBUG << "Start Client ACK - StratID [" << ClientOrder.GetFixTag(FIX_TAG_STRATID_BO)
								<< "] Client OrderID [" << ClientOrder.GetHandle()
								<< "] Client RefOrdId [" << ClientOrder.GetRefOrdId()
								<< "] Client NewOrderId [" << ClientOrder.GetNewOrderId()
								<< "] Client CommonOrderId [" << ClientOrder.GetCommonOrderId()
								<< "] Client GetPrimOrdId [" << ClientOrder.GetPrimOrdId()
								<< "] Client GetOrigOrderId [" << ClientOrder.GetOrigOrderId()
								<< "] Client Symbol [" << ClientOrder.GetSymbol()
								<< "]"
								<<std::endl);

	if (eOrdState == STATE_REPLACED)
	{
		_STRAT_LOG_VERB_(CINFO << "Callback confirming RPLD state. Client OrderID [" << ClientExec.GetOrderID() << "]" << std::endl);

		_STRAT_LOG_DBUG_(PrintStratOrderMapRpl(gmStratMapRpl));

		// Step 1: Find StratParams in gmStratMap and gmStratMapRpl
		// Get pStratParams and pStratParamsRpl for further processing
		long nStratId = atol(ClientOrder.GetFixTag(FIX_TAG_STRATID_BO));
		STRATMAPITER mStIterRpl = gmStratMapRpl.find(nStratId);
		STRATMAPITER mStIter = gmStratMap.find(nStratId);

		if((mStIter == gmStratMap.end()) ||(mStIterRpl == gmStratMapRpl.end()))
		{
			if (mStIter == gmStratMap.end())
			{
				_STRAT_LOG_DBUG_ (CDEBUG << "StratID [" << nStratId << "] not found in gmStratMap!" << std::endl);
			}
			if (mStIterRpl == gmStratMapRpl.end())
			{
				_STRAT_LOG_DBUG_ (CDEBUG << "StratID [" << nStratId << "] not found in gmStratMapRpl!" << std::endl);
			}
			return FAILURE;
		}

		StratParams *pStratParamsRpl = (StratParams*) mStIterRpl->second;
		StratParams *pStratParams = (StratParams*) mStIter->second;

		if(!pStratParams || !pStratParamsRpl)
		{
	        _STRAT_LOG_DBUG_(CDEBUG << "Invalid pointer pStratParams or pStratParamsRpl " << std::endl);
			return FAILURE;
		}

		_STRAT_LOG_DBUG_ (CWARN << "RPLD : StratID [" << nStratId
								<< "] Client OrderID [" << ClientOrder.GetHandle()
								<< "] Client Symbol [" << ClientOrder.GetSymbol()
								<< "]" << std::endl);

	   // Step 2: Check RPL-ACK for each client orders in the strat
	   if(!strcmp(pStratParamsRpl->szSymbolQuote, ClientOrder.GetSymbol()) &&
			   ClientOrder.GetSide() == SIDE_BUY)
	   {

		   _STRAT_LOG_VERB_(CWARN << "RPLD : StratID [" << nStratId
									<< "] Client OrderID [" << ClientOrder.GetHandle()
									<< "] Client Side [" << ClientOrder.GetSide()
									<< "] Client Symbol [" << ClientOrder.GetSymbol()
									<< "] : Bid Quote Replaced!" << std::endl);

		   pStratParamsRpl->bBidQuoteRpld = true;
	   }
	   else if(!strcmp(pStratParamsRpl->szSymbolQuote, ClientOrder.GetSymbol()) &&
			   ClientOrder.GetSide() == SIDE_SELL)
	   {
		   _STRAT_LOG_VERB_(CWARN << "RPLD : StratID [" << nStratId
									<< "] Client OrderID [" << ClientOrder.GetHandle()
									<< "] Client Side [" << ClientOrder.GetSide()
									<< "] Client Symbol [" << ClientOrder.GetSymbol()
									<< "] : Offer Quote Replaced!" << std::endl);

		   pStratParamsRpl->bOfferQuoteRpld = true;
	   }
	   else if(!strcmp(pStratParamsRpl->szSymbolHedge, ClientOrder.GetSymbol()) &&
			   ClientOrder.GetSide() == SIDE_BUY)
	   {
		   _STRAT_LOG_VERB_(CWARN << "RPLD : StratID [" << nStratId
									<< "] Client OrderID [" << ClientOrder.GetHandle()
									<< "] Client Side [" << ClientOrder.GetSide()
									<< "] Client Symbol [" << ClientOrder.GetSymbol()
									<< "] : Bid Hedge Replaced!" << std::endl);

		   pStratParamsRpl->bBidHedgeRpld = true;
	   }
	   else if(!strcmp(pStratParamsRpl->szSymbolHedge, ClientOrder.GetSymbol()) &&
			   ClientOrder.GetSide() == SIDE_SELL)
	   {
		   _STRAT_LOG_VERB_(CWARN << "RPLD : StratID [" << nStratId
									<< "] Client OrderID [" << ClientOrder.GetHandle()
									<< "] Client Side [" << ClientOrder.GetSide()
									<< "] Client Symbol [" << ClientOrder.GetSymbol()
									<< "] : Offer Hedge Replaced!" << std::endl);

		   pStratParamsRpl->bOfferHedgeRpld = true;
	   }
	   else
	   {
		   _STRAT_LOG_VERB_ (CERR << "Failed to match PrimOrdId [" << ClientOrder.GetPrimOrdId()
				   	   	   	   	   << "] or Symbol [" << ClientOrder.GetSymbol() << "]!" << std::endl);
		   return FAILURE;
	   }

	  // Step 3:Do the conditional processing ONLY after receiving RPL Client Orders on ALL the client orders
	    if (pStratParamsRpl->bBidQuoteRpld && pStratParamsRpl->bOfferQuoteRpld && pStratParamsRpl->bBidHedgeRpld && pStratParamsRpl->bOfferHedgeRpld)
		{
	    	//Update Params: Rpl Counts, Avg Execs, etc.
	    	UpdateStratParamWithClientRpld(pStratParams, pStratParamsRpl);

	    	//Update Maps
			gmStratMap.erase(mStIter);
			delete pStratParams;

			pStratParamsRpl->bStratRunning = (bool)atoi(ClientOrder.GetFixTag(FIX_TAG_RUNNING_BO));

			gmStratMap.insert(pair<int, StratParams*>(nStratId, pStratParamsRpl));
			gmStratMapRpl.erase(mStIterRpl);

	        _STRAT_LOG_VERB_(CINFO << "StratID [" << nStratId
	        		<< "] -> Committed the RPL for the StratParams map!"
	        		<< std::endl);

	        _STRAT_LOG_VERB_(PrintStratOrderMap(gmStratMap, nStratId));
	        _STRAT_LOG_DBUG_(PrintStratOrderMapRpl(gmStratMapRpl));
		}
	}
	else if (eOrdState == STATE_OPEN)
	{
	        _STRAT_LOG_VERB_ (CWARN << "Callback confirming ACK state. Client OrderID [" << ClientExec.GetOrderID() << "]" << std::endl);
	}
	else if (eOrdState == STATE_PENDING_NEW)
	{
			_STRAT_LOG_VERB_ (CWARN << "Callback confirming PEND-NEW state. Client OrderID [" << ClientExec.GetOrderID() << "]" << std::endl);
	        ClientOrder.SendAck();
	}
	else
	{
		_STRAT_LOG_VERB_ (CERR << "Callback in wrong state. Client OrderID [" << ClientExec.GetOrderID()
	        		<< "] -> State [" << StateToStr(eOrdState) << "]" << std::endl);
		return FAILURE;
	}
	return SUCCESS;
}

int BidOfferSpread::OnClientSendOut(OMRULESEXPORT::CLIENT_EXEC&){return SUCCESS;}

int BidOfferSpread::OnClientSendReject(OMRULESEXPORT::CLIENT_EXEC&){return SUCCESS;}

int BidOfferSpread::OnClientSendPendingRpl(OMRULESEXPORT::CLIENT_EXEC&){return SUCCESS;}

//Street Order callbacks
int BidOfferSpread::OnStreetOrdNew(STREET_ORDER& streetOrder, CLIENT_ORDER& parentCLIENT_ORDER){return SUCCESS;}

int BidOfferSpread::OnStreetOrdValidNew(STREET_ORDER& streetOrder, CLIENT_ORDER& parentOrder)
{
	_STRAT_LOG_DBUG_(CDEBUG << "StreetOrder ID [" << streetOrder.GetHandle()
							<< "] ClientOrder ID [" << parentOrder.GetHandle()
							<< "] : Validating Price Cross."
							<< std::endl);

	return	OrderPriceCrossCheck(streetOrder);
}

int BidOfferSpread::OnStreetOrdRpl(STREET_ORDER& streetOrder, CLIENT_ORDER& parentCLIENT_ORDER){return SUCCESS;}

int BidOfferSpread::OnStreetOrdValidRpl(STREET_ORDER& streetOrder, CLIENT_ORDER& parentOrder){return SUCCESS;}

int BidOfferSpread::OnStreetOrdCancel(STREET_ORDER& streetOrder, CLIENT_ORDER& parentCLIENT_ORDER)
{
	UpdateStratQuoteCxlRplCount(streetOrder, parentCLIENT_ORDER);

	_STRAT_LOG_DBUG_(CDEBUG << "StreetOrder ID [" << streetOrder.GetHandle()
							<<"] sending cancel!"
							<< std::endl);

	return SUCCESS;
}

int BidOfferSpread::OnStreetOrdValidCancel(STREET_ORDER& streetOrder, CLIENT_ORDER& parentOrder)
{
	if(!CanReplace(streetOrder))
	{
		CERR << "Cannot Cancel Street Order [" << streetOrder.GetHandle()
				<<"] with Client Order [" << parentOrder.GetHandle()
				<<"] , OrderState [" << StateToStr(streetOrder.GetOrderState())
				<<"]"
				<< std::endl;

		return FAILURE;
	}

	return SUCCESS;
}

int BidOfferSpread::OnStreetOrdAck(STREET_ORDER& streetOrder)
{
	_STRAT_LOG_VERB_(CWARN << "StreetOrder ID [" << streetOrder.GetHandle()
					<<"] got ACK!."
					<< std::endl);

	return SUCCESS;
}

int BidOfferSpread::OnStreetExec(STREET_EXEC& StreetExec, CLIENT_ORDER& parentOrder)
{

	if(StreetExec.GetBrokerOrdId())
	{
		StreetExec.SetFixTag(37,StreetExec.GetBrokerOrdId());
	}
	if(StreetExec.GetExecId())
	{
		StreetExec.SetFixTag(17,StreetExec.GetExecId());
	}

	PrintStreetExec(StreetExec, parentOrder);

	if (ExecsRiskLimitControl(StreetExec, parentOrder) == FAILURE)
	{
		// IF ExecsRiskLimitControl fails, Strat is already stopped. So just return FAILURE.
		return FAILURE;
	}
	else
	{
		CINFO << "StreetExec Handle [" << StreetExec.GetHandle()
				<< "] Symbol [" << StreetExec.GetSymbol()
				<< "] Side [" << StreetExec.GetSide()
				<< "] LegID [" << StreetExec.GetClientId2()
				<< "] OrderState [" <<  StateToStr(StreetExec.GetOrderState())
				<< "] TotalLvs [" << StreetExec.GetTotalLvs()
				<< "] LastFillQty [" << StreetExec.GetLastFillQty()
				<< "] LastFillPrice [" << StreetExec.GetLastFillPrice()
				<< "] - Passed ExecsRiskLimit Check."
				<< std::endl;

		if ((StreetExec.GetOrderState()==STATE_PARTIAL || StreetExec.GetOrderState()==STATE_FILLED)
				&& StreetExec.GetLastFillQty() != 0 )
		{
			_STRAT_LOG_VERB_(PrintSHFEOrdRplMap(gmSHFEOrdRplMap));

			SHFEORDERRPLMAPITER mShfeRplIter = gmSHFEOrdRplMap.find(string(StreetExec.GetHandle()));
			if (mShfeRplIter != gmSHFEOrdRplMap.end())
			{
				CWARN << "SHFE StreetOrder ID [" << StreetExec.GetHandle()
						<< "] Symbol [" << StreetExec.GetSymbol()
						<< "] Side [" << StreetExec.GetSide()
						<< "] got executions while Pending-Cancel! : Remove Order [" << StreetExec.GetHandle()
						<< "] from gmSHFEOrdRplMap."
						<< std::endl;

				gmSHFEOrdRplMap.erase(mShfeRplIter);

				_STRAT_LOG_DBUG_(PrintSHFEOrdRplMap(gmSHFEOrdRplMap));
			}
		}

		if (StreetExec.GetClientId2() == LEGID_BO_BQ || StreetExec.GetClientId2() == LEGID_BO_OQ)
		{
			CWARN << "StreetExec Handle [" << StreetExec.GetHandle()
					<< "] Symbol [" << StreetExec.GetSymbol()
					<< "] Side [" << StreetExec.GetSide()
					<< "] LegID [" << StreetExec.GetClientId2()
					<< "] : Execs on Quote Order! Proceed to Adjust Hedge Size!"
					<< std::endl;

			HandleHedgeOnQuoteExecs(StreetExec, parentOrder);
		}
	}

	return SUCCESS;
}

int BidOfferSpread::OnStreetOrdRej(STREET_ORDER& streetOrder, CLIENT_ORDER& parentCLIENT_ORDER)
{
	_STRAT_LOG_VERB_(CWARN << "Street Order ID [" << streetOrder.GetHandle()
							<< "] with RefOrdId [" << streetOrder.GetRefOrdId()
							<< "] Symbol [" << streetOrder.GetSymbol()
							<< "] Side [" << streetOrder.GetSide()
							<< "] got REJECTED!"
							<< std::endl);

	char pszFixTag8101[512] = "";
	strncpy(pszFixTag8101,streetOrder.GetFixTag(FIX_TAG_OR_MSG),sizeof(pszFixTag8101));
	if(pszFixTag8101 && strlen(pszFixTag8101)>0)
	{
		_STRAT_LOG_VERB_ (CWARN << " ~~EXCHANGE~~ 8101 Message received : " << pszFixTag8101 << std::endl);
	}
	else
	{
		_STRAT_LOG_DBUG_ (CDEBUG << " ~~EXCHANGE~~ No Message received in 8101 ! " << std::endl);
	}

	STREET_ORDER_CONTAINER SOrdContainer;
	STREET_ORDER OrigStreetOrder;

	_STRAT_LOG_DBUG_(PrintSHFEOrdRplMap(gmSHFEOrdRplMap));

	SHFEORDERRPLMAPITER mShfeRplIter = gmSHFEOrdRplMap.find(string(streetOrder.GetRefOrdId()));

	if (mShfeRplIter != gmSHFEOrdRplMap.end())
	{
		CWARN << "SHFE Cancel Request StreetOrder ID [" << streetOrder.GetHandle()
				<< "] with RefOrdId [" << streetOrder.GetRefOrdId()
				<< "] got REJECTED! : Handle SHFE Replace Reject."
				<< std::endl;


		if (HandleSHFEReplace(streetOrder, 0, 0.0) != SUCCESS)
		{
			_STRAT_LOG_VERB_(CERR << "SHFE Cancel Request StreetOrder ID [" << streetOrder.GetHandle()
									<< "] with RefOrdId [" << streetOrder.GetRefOrdId()
									<< "] got REJECTED! : Error in processing SHFE Replace Reject."
									<< std::endl);
			return FAILURE;
		}
		else
		{
			_STRAT_LOG_VERB_(CWARN << "SHFE Cancel Request StreetOrder ID [" << streetOrder.GetHandle()
									<< "] with RefOrdId [" << streetOrder.GetRefOrdId()
									<< "] got REJECTED! : Successfully processed SHFE Replace Reject."
									<< std::endl);
		}
	}

	return SUCCESS;
}

int BidOfferSpread::OnStreetOrdOut(STREET_ORDER& so)
{
	if (so.GetOrderState() == STATE_FILLED)
	{
		_STRAT_LOG_VERB_(CWARN << "StreetOrder ID [" << so.GetHandle()
				<< "] with State [" << StateToStr(so.GetOrderState())
				<< "] was OUTED! Symbol [" << so.GetSymbol()
				<< "] Side [" << so.GetSide()
				<< "] - Ignore OUT!"
				<< std::endl);
		return SUCCESS;
	}


	_STRAT_LOG_VERB_(PrintSHFEOrdRplMap(gmSHFEOrdRplMap));

	SHFEORDERRPLMAPITER mShfeRplIter = gmSHFEOrdRplMap.find(string(so.GetHandle()));
	if (mShfeRplIter != gmSHFEOrdRplMap.end())
	{
		_STRAT_LOG_DBUG_(CWARN << "SHFE Cancel Request StreetOrder ID [" << so.GetHandle()
				<<"] got OUT! : Handle SHFE RPLD."
				<< std::endl);

		if (HandleSHFEReplace(so, 0, 0.0) != SUCCESS)
		{
			_STRAT_LOG_VERB_(CERR << "SHFE Cancel Request StreetOrder ID [" << so.GetHandle()
									<<"] got OUT! : Error in processing SHFE RPLD."
									<< std::endl);
			return FAILURE;
		}
		else
		{
			_STRAT_LOG_VERB_(CWARN << "SHFE Cancel Request StreetOrder ID [" << so.GetHandle()
									<<"] got OUT! :  Successfully processed SHFE RPLD."
									<< std::endl);
		}

	}

	return SUCCESS;
}

int BidOfferSpread::OnStreetStatusReport(STREET_EXEC& StreetExec, CLIENT_ORDER& ParentOrder)
{
	PrintStreetExec(StreetExec, ParentOrder);

	STREET_ORDER StreetOrder;
	StreetOrder = StreetExec.GetStreetOrder();

	char pszFixTag8101[512] = "";
	strncpy(pszFixTag8101,StreetExec.GetFixTag(FIX_TAG_OR_MSG),sizeof(pszFixTag8101));
	if(pszFixTag8101 && strlen(pszFixTag8101)>0)
	{
		_STRAT_LOG_VERB_ (CWARN << " ~~EXCHANGE~~ 8101 Message received : " << pszFixTag8101 << std::endl);
	}
	else
	{
		_STRAT_LOG_VERB_ (CWARN << " ~~EXCHANGE~~ No Message received in 8101 ! " << std::endl);
	}

	if (StreetExec.GetOrderState() == STATE_CANCELLED && StreetExec.GetTotalLvs() == 0)
	{
		CINFO << "StreetExecs StreetOrder ID [" << StreetExec.GetHandle()
				<< "with OrderState [" <<  StateToStr(StreetExec.GetOrderState())
				<< "] and TotalLvs [" << StreetExec.GetTotalLvs()
				<< "] : Order is OUTED!"
				<< std::endl;

		_STRAT_LOG_VERB_(PrintSHFEOrdRplMap(gmSHFEOrdRplMap));

		SHFEORDERRPLMAPITER mShfeRplIter = gmSHFEOrdRplMap.find(string(StreetOrder.GetHandle()));
		if (mShfeRplIter != gmSHFEOrdRplMap.end())
		{
			// If outed order is in SHFEOrdRplMap, proceed to hand SHFE replace
			_STRAT_LOG_VERB_(CWARN << "Cancel Request for StreetOrder ID [" << StreetOrder.GetHandle()
					<<"] got OUT! : Handle SHFE RPLD."
					<< std::endl);

			if (HandleSHFEReplace(StreetOrder, 0, 0.0) != SUCCESS)
			{
				_STRAT_LOG_VERB_(CERR << "SHFE Cancel Request StreetOrder ID [" << StreetOrder.GetHandle()
										<<"] got OUT! : Error in processing SHFE RPLD."
										<< std::endl);
				return FAILURE;
			}
			else
			{
				_STRAT_LOG_VERB_(CWARN << "SHFE Cancel Request StreetOrder ID [" << StreetOrder.GetHandle()
										<<"] got OUT! :  Successfully processed SHFE RPLD."
										<< std::endl);
			}
		}
		else
		{
			// Else proceed to check for client order state and working size
			_STRAT_LOG_VERB_(CWARN << "Cancel Request for StreetOrder ID [" << StreetOrder.GetHandle()
					<<"] got OUT! : Treated as EVENT_STREET_OUT."
					<< std::endl);

			if(ParentOrder.isReplacePending() && ParentOrder.GetWorkingSize() == 0)
			{
				CWARN << "StreetOrder ID [" << StreetOrder.GetHandle()
							<<"] linked to Client Order ID [" << ParentOrder.GetHandle()
							<<"] with ReplacePending [" << ParentOrder.isReplacePending()
							<<"] and WorkingSize [" << ParentOrder.GetWorkingSize()
							<<"] : All street orders are CANCELED - Sending RPLD on Client Order!"
							<< std::endl;

				ParentOrder.SendReplaced();
			}
			else if (ParentOrder.isCancelPending() && ParentOrder.GetWorkingSize() == 0)
			{
				CWARN << "StreetOrder ID [" << StreetOrder.GetHandle()
							<<"] linked to Client Order ID [" << ParentOrder.GetHandle()
							<<"] with CancelPending [" << ParentOrder.isCancelPending()
							<<"] and WorkingSize [" << ParentOrder.GetWorkingSize()
							<<"] : All street orders are CANCELED - Sending CXLD on Client Order!"
							<< std::endl;

				ParentOrder.SendOut();
			}
			else
			{
				_STRAT_LOG_DBUG_(CDEBUG << "StreetOrder ID [" << StreetOrder.GetHandle()
							<<"] linked to Client Order ID [" << ParentOrder.GetHandle()
							<<"] with ReplacePending [" << ParentOrder.isReplacePending()
							<<"] CancelPending [" << ParentOrder.isCancelPending()
							<<"] and WorkingSize [" << ParentOrder.GetWorkingSize()
							<<"] : Not All street orders are CANCELED yet."
							<< std::endl);
			}
		}
	}
	else if ((StreetExec.GetOrderState()==STATE_PARTIAL || StreetExec.GetOrderState()==STATE_FILLED)
				&& StreetExec.GetLastFillQty() != 0 )
	{
		CINFO << "StreetExecs Handle [" << StreetExec.GetHandle()
								<< "] with OrderState [" <<  StateToStr(StreetExec.GetOrderState())
								<< "] and TotalLvs [" << StreetExec.GetTotalLvs()
								<< "] -> LastFillQty [" << StreetExec.GetLastFillQty()
								<< "] at Price [" << StreetExec.GetLastFillPrice()
								<< "] : Order got executions!"
								<< std::endl;

		if (ExecsRiskLimitControl(StreetExec, ParentOrder) == FAILURE)
		{
			// IF ExecsRiskLimitControl fails, Strat is already stopped. So just return FAILURE.
			return FAILURE;
		}
		else
		{
			CINFO << "StreetExec Handle [" << StreetExec.GetHandle()
					<< "] Symbol [" << StreetExec.GetSymbol()
					<< "] Side [" << StreetExec.GetSide()
					<< "] LegID [" << StreetExec.GetClientId2()
					<< "] OrderState [" <<  StateToStr(StreetExec.GetOrderState())
					<< "] TotalLvs [" << StreetExec.GetTotalLvs()
					<< "] LastFillQty [" << StreetExec.GetLastFillQty()
					<< "] LastFillPrice [" << StreetExec.GetLastFillPrice()
					<< "] - Passed ExecsRiskLimit Check."
					<< std::endl;

			// If receive executions while pending cancel, abort replace process
			_STRAT_LOG_VERB_(PrintSHFEOrdRplMap(gmSHFEOrdRplMap));

			SHFEORDERRPLMAPITER mShfeRplIter = gmSHFEOrdRplMap.find(string(StreetExec.GetHandle()));
			if (mShfeRplIter != gmSHFEOrdRplMap.end())
			{
				CWARN << "SHFE StreetOrder ID [" << StreetExec.GetHandle()
						<< "] Symbol [" << StreetExec.GetSymbol()
						<< "] Side [" << StreetExec.GetSide()
						<< "] got executions while Pending-Cancel! : Remove Order [" << StreetExec.GetHandle()
						<< "] from gmSHFEOrdRplMap."
						<< std::endl;

				gmSHFEOrdRplMap.erase(mShfeRplIter);

				_STRAT_LOG_DBUG_(PrintSHFEOrdRplMap(gmSHFEOrdRplMap));
			}


			if (StreetExec.GetClientId2() == LEGID_BO_BQ || StreetExec.GetClientId2() == LEGID_BO_OQ)
			{
				CWARN << "StreetExec Handle [" << StreetExec.GetHandle()
						<< "] Symbol [" << StreetExec.GetSymbol()
						<< "] Side [" << StreetExec.GetSide()
						<< "] LegID [" << StreetExec.GetClientId2()
						<< "] : Execs on Quote Order! Proceed to Adjust Hedge Size!"
						<< std::endl;

				HandleHedgeOnQuoteExecs(StreetExec, ParentOrder);
			}
		}
	}
	else
	{
		_STRAT_LOG_DBUG_(CWARN << "StreetExecs StreetOrder ID [" << StreetExec.GetHandle()
				<< "] with OrderState [" <<  StateToStr(StreetExec.GetOrderState())
				<< "] and TotalLvs [" << StreetExec.GetTotalLvs()
				<< "] LastFillQty [" << StreetExec.GetLastFillQty()
				<< "] : No Action was taken!"
				<< std::endl);
	}

	return SUCCESS;
}

int BidOfferSpread::OnStreetNewReportStat(STREET_EXEC& StreetExec, CLIENT_ORDER& ParentOrder)
{
	PrintStreetExec(StreetExec, ParentOrder);

	STREET_ORDER StreetOrder;
	StreetOrder = StreetExec.GetStreetOrder();

	char pszFixTag8101[512] = "";
	strncpy(pszFixTag8101,StreetExec.GetFixTag(FIX_TAG_OR_MSG),sizeof(pszFixTag8101));
	if(pszFixTag8101 && strlen(pszFixTag8101)>0)
	{
		_STRAT_LOG_VERB_ (CWARN << " ~~EXCHANGE~~ 8101 Message received : " << pszFixTag8101 << std::endl);
	}
	else
	{
		_STRAT_LOG_VERB_ (CDEBUG << " ~~EXCHANGE~~ No Message received in 8101 ! " << std::endl);
	}

	if (StreetExec.GetOrderState() == STATE_CANCELLED && StreetExec.GetTotalLvs() == 0)
	{
		CINFO << "StreetExecs StreetOrder ID [" << StreetExec.GetHandle()
				<< "with OrderState [" <<  StateToStr(StreetExec.GetOrderState())
				<< "] and TotalLvs [" << StreetExec.GetTotalLvs()
				<< "] : Order is OUTED!"
				<< std::endl;

		_STRAT_LOG_DBUG_(PrintSHFEOrdRplMap(gmSHFEOrdRplMap));

		SHFEORDERRPLMAPITER mShfeRplIter = gmSHFEOrdRplMap.find(string(StreetOrder.GetHandle()));
		if (mShfeRplIter != gmSHFEOrdRplMap.end())
		{
			// If outed order is in SHFEOrdRplMap, proceed to hand SHFE replace
			_STRAT_LOG_VERB_(CWARN << "Cancel Request for StreetOrder ID [" << StreetOrder.GetHandle()
					<<"] got OUT! : Handle SHFE RPLD."
					<< std::endl);

			if (HandleSHFEReplace(StreetOrder, 0, 0.0) != SUCCESS)
			{
				_STRAT_LOG_VERB_(CERR << "SHFE Cancel Request StreetOrder ID [" << StreetOrder.GetHandle()
										<<"] got OUT! : Error in processing SHFE RPLD."
										<< std::endl);
				return FAILURE;
			}
			else
			{
				_STRAT_LOG_VERB_(CWARN << "SHFE Cancel Request StreetOrder ID [" << StreetOrder.GetHandle()
										<<"] got OUT! :  Successfully processed SHFE RPLD."
										<< std::endl);
			}
		}
		else
		{
			// Else proceed to check for client order state and working size
			_STRAT_LOG_VERB_(CWARN << "Cancel Request for StreetOrder ID [" << StreetOrder.GetHandle()
					<<"] got OUT! : Treated as EVENT_STREET_OUT."
					<< std::endl);

			if(ParentOrder.isReplacePending() && ParentOrder.GetWorkingSize() == 0)
			{
				CWARN << "StreetOrder ID [" << StreetOrder.GetHandle()
							<<"] linked to Client Order ID [" << ParentOrder.GetHandle()
							<<"] with ReplacePending [" << ParentOrder.isReplacePending()
							<<"] and WorkingSize [" << ParentOrder.GetWorkingSize()
							<<"] : All street orders are CANCELED - Sending RPLD on Client Order!"
							<< std::endl;

				ParentOrder.SendReplaced();
			}
			else if (ParentOrder.isCancelPending() && ParentOrder.GetWorkingSize() == 0)
			{
				CWARN << "StreetOrder ID [" << StreetOrder.GetHandle()
							<<"] linked to Client Order ID [" << ParentOrder.GetHandle()
							<<"] with CancelPending [" << ParentOrder.isCancelPending()
							<<"] and WorkingSize [" << ParentOrder.GetWorkingSize()
							<<"] : All street orders are CANCELED - Sending CXLD on Client Order!"
							<< std::endl;

				ParentOrder.SendOut();
			}
			else
			{
				_STRAT_LOG_DBUG_(CDEBUG << "StreetOrder ID [" << StreetOrder.GetHandle()
							<<"] linked to Client Order ID [" << ParentOrder.GetHandle()
							<<"] with ReplacePending [" << ParentOrder.isReplacePending()
							<<"] CancelPending [" << ParentOrder.isCancelPending()
							<<"] and WorkingSize [" << ParentOrder.GetWorkingSize()
							<<"] : Not Sending ACK on Client Order."
							<< std::endl);
			}
		}

	}
	else if ((StreetExec.GetOrderState()==STATE_PARTIAL || StreetExec.GetOrderState()==STATE_FILLED)
				&& StreetExec.GetLastFillQty() != 0 )
	{
		CINFO << "StreetExecs Handle [" << StreetExec.GetHandle()
								<< "] with OrderState [" <<  StateToStr(StreetExec.GetOrderState())
								<< "] and TotalLvs [" << StreetExec.GetTotalLvs()
								<< "] -> LastFillQty [" << StreetExec.GetLastFillQty()
								<< "] at Price [" << StreetExec.GetLastFillPrice()
								<< "] : Order got executions!"
								<< std::endl;

		if (ExecsRiskLimitControl(StreetExec, ParentOrder) == FAILURE)
		{
			// IF ExecsRiskLimitControl fails, Strat is already stopped. So just return FAILURE.
			return FAILURE;
		}
		else
		{
			CINFO << "StreetExec Handle [" << StreetExec.GetHandle()
					<< "] Symbol [" << StreetExec.GetSymbol()
					<< "] Side [" << StreetExec.GetSide()
					<< "] LegID [" << StreetExec.GetClientId2()
					<< "] OrderState [" <<  StateToStr(StreetExec.GetOrderState())
					<< "] TotalLvs [" << StreetExec.GetTotalLvs()
					<< "] LastFillQty [" << StreetExec.GetLastFillQty()
					<< "] LastFillPrice [" << StreetExec.GetLastFillPrice()
					<< "] - Passed ExecsRiskLimit Check."
					<< std::endl;


			// If receive executions while pending cancel, abort replace process
			_STRAT_LOG_VERB_(PrintSHFEOrdRplMap(gmSHFEOrdRplMap));

			SHFEORDERRPLMAPITER mShfeRplIter = gmSHFEOrdRplMap.find(string(StreetExec.GetHandle()));
			if (mShfeRplIter != gmSHFEOrdRplMap.end())
			{
				CWARN << "SHFE StreetOrder ID [" << StreetExec.GetHandle()
						<< "] Symbol [" << StreetExec.GetSymbol()
						<< "] Side [" << StreetExec.GetSide()
						<< "] got executions while Pending-Cancel! : Remove Order [" << StreetExec.GetHandle()
						<< "] from gmSHFEOrdRplMap."
						<< std::endl;

				gmSHFEOrdRplMap.erase(mShfeRplIter);

				_STRAT_LOG_DBUG_(PrintSHFEOrdRplMap(gmSHFEOrdRplMap));
			}

			if (StreetExec.GetClientId2() == LEGID_BO_BQ || StreetExec.GetClientId2() == LEGID_BO_OQ)
			{
				CWARN << "StreetExec Handle [" << StreetExec.GetHandle()
						<< "] Symbol [" << StreetExec.GetSymbol()
						<< "] Side [" << StreetExec.GetSide()
						<< "] LegID [" << StreetExec.GetClientId2()
						<< "] : Execs on Quote Order! Proceed to Adjust Hedge Size!"
						<< std::endl;

				HandleHedgeOnQuoteExecs(StreetExec, ParentOrder);
			}
		}
	}
	else
	{
		_STRAT_LOG_DBUG_(CWARN << "StreetExecs StreetOrder ID [" << StreetExec.GetHandle()
				<< "] with OrderState [" <<  StateToStr(StreetExec.GetOrderState())
				<< "] and TotalLvs [" << StreetExec.GetTotalLvs()
				<< "] LastFillQty [" << StreetExec.GetLastFillQty()
				<< "] : No Action was taken!"
				<< std::endl);
	}

	return SUCCESS;
}

int BidOfferSpread::OnStreetDiscardOrdCancel(STREET_ORDER& StreetOrd, CLIENT_ORDER& ParentOrder)
{
	_STRAT_LOG_VERB_(CWARN << "StreetOrd ID [" << StreetOrd.GetHandle()
							<< "] Symbol [" << StreetOrd.GetSymbol()
							<< "] Side [" << StreetOrd.GetSide()
							<< "] Leaves [" << StreetOrd.GetTotalLvs()
							<< "] with State [" << StateToStr(StreetOrd.GetOrderState())
							<< "!"
							<< std::endl);

	char pszFixTag8101[512] = "";
	strncpy(pszFixTag8101,StreetOrd.GetFixTag(FIX_TAG_OR_MSG),sizeof(pszFixTag8101));
	if(pszFixTag8101 && strlen(pszFixTag8101)>0)
	{
		_STRAT_LOG_VERB_ (CWARN << " ~~EXCHANGE~~ 8101 Message received : " << pszFixTag8101 << std::endl);
	}
	else
	{
		_STRAT_LOG_DBUG_ (CDEBUG << " ~~EXCHANGE~~ No Message received in 8101 ! " << std::endl);
	}

	if (StreetOrd.GetOrderState() == STATE_FILLED && StreetOrd.GetTotalLvs() == 0)
	{
		_STRAT_LOG_VERB_(CWARN << "StreetOrder ID [" << StreetOrd.GetHandle()
				<< "] with State [" << StateToStr(StreetOrd.GetOrderState())
				<< "]! Symbol [" << StreetOrd.GetSymbol()
				<< "] Side [" << StreetOrd.GetSide()
				<< "] - Ignore DiscardOrdCxl!"
				<< std::endl);
		return SUCCESS;
	}
	else if (StreetOrd.GetOrderState() != STATE_CANCELLED && StreetOrd.GetTotalLvs() != 0)
	{
		_STRAT_LOG_DBUG_(CDEBUG << "StreetOrd ID [" << StreetOrd.GetHandle()
								<< "] Symbol [" << StreetOrd.GetSymbol()
								<< "] Side [" << StreetOrd.GetSide()
								<< "] Leaves [" << StreetOrd.GetTotalLvs()
								<< "] with State [" << StateToStr(StreetOrd.GetOrderState())
								<< "] : Order Cancel Rejected!"
								<< std::endl);

		_STRAT_LOG_VERB_(PrintSHFEOrdRplMap(gmSHFEOrdRplMap));

		SHFEORDERRPLMAPITER mShfeRplIter = gmSHFEOrdRplMap.find(string(StreetOrd.GetHandle()));
		if (mShfeRplIter != gmSHFEOrdRplMap.end())
		{
			CWARN << "SHFE Cancel Request for StreetOrder ID [" << StreetOrd.GetHandle()
					<< "] Symbol [" << StreetOrd.GetSymbol()
					<< "] Side [" << StreetOrd.GetSide()
					<< "] got DISCARDED! : Remove Order [" << StreetOrd.GetHandle()
					<< "] from gmSHFEOrdRplMap and Update Strat State."
					<< std::endl;

			gmSHFEOrdRplMap.erase(mShfeRplIter);

			_STRAT_LOG_DBUG_(PrintSHFEOrdRplMap(gmSHFEOrdRplMap));

			return SUCCESS;
		}
		else
		{
			CERR << "Cancel Request for StreetOrder ID [" << StreetOrd.GetHandle()
					<<"] got DISCARDED! : Ignored because OrderID NOT found in gmSHFEOrdRplMap!"
					<< std::endl;
			return FAILURE;
		}
	}
	if (StreetOrd.GetOrderState() != STATE_CANCELLED && StreetOrd.GetTotalLvs() == 0)
	{
		_STRAT_LOG_VERB_(CERR << "StreetOrd ID [" << StreetOrd.GetHandle()
								<< "] Symbol [" << StreetOrd.GetSymbol()
								<< "] Side [" << StreetOrd.GetSide()
								<< "] Leaves [" << StreetOrd.GetTotalLvs()
								<< "] with State [" << StateToStr(StreetOrd.GetOrderState())
								<< "] : NO-LVS!"
								<< std::endl);
		return FAILURE;
	}

	return SUCCESS;
}

int BidOfferSpread::OnTimer(EVENT_DATA& eventData){return SUCCESS;}

//Custom
StratParams* BidOfferSpread::GetActiveStratParamsByClientOrderId(const char* pszClientOrderId)
{
	// STEP 1 : Validate StratID
	STRATIDMAPITER mStIdIter = gmStratIdMap.find(string(pszClientOrderId));
	long nStratId = -1;
	if(mStIdIter != gmStratIdMap.end())
	{
		nStratId = (long) mStIdIter->second;
		_STRAT_LOG_DBUG_(CDEBUG << "Found Strat ID [" << nStratId
								<< "] with OrderID [" << pszClientOrderId
								<< "]"
								<< std::endl);
	}
	else
	{
		_STRAT_LOG_VERB_(CERR << "OrderID [" << pszClientOrderId << "] not found in gmStratIdMap!" << std::endl);
		return NULL;
	}

	// STEP 2: Get StratParams from Global Map gmStratMap
	STRATMAPITER mStIter = gmStratMap.find(nStratId);
	StratParams *pStratParams = NULL;
	if(mStIter != gmStratMap.end())
	{
		pStratParams = (StratParams*) mStIter->second;
		_STRAT_LOG_DBUG_(CDEBUG << "Found StratParams with StratID [" << nStratId << "]"  << std::endl);
	}
	else
	{
		_STRAT_LOG_VERB_(CWARN << "StratID [" << nStratId << "] not found in gmStratMap!" << std::endl);
		return NULL;
	}

	if(!pStratParams)
	{
		_STRAT_LOG_VERB_(CWARN << "Invalid StratParam Pointer!" << std::endl);
		return NULL;
	}

	return pStratParams;
}

bool BidOfferSpread::IsValidMarketTick(MTICK& mtick)
{
	_STRAT_LOG_VERB_(CWARN << "Market data: Symbol [" << mtick.GetSymbol()
							<<"] : Bid [" << mtick.GetBid()
							<<"] BidSize [" << mtick.GetBidSize()
							<< "] / Ask [" << mtick.GetAsk()
							<< "] AskSize [" << mtick.GetAskSize()
							<< "]"
							<< std::endl);

	if(mtick.GetBid() < g_dEpsilon ||  mtick.GetAsk() < g_dEpsilon || mtick.GetBid() - mtick.GetAsk() > g_dEpsilon)
	{
		_STRAT_LOG_VERB_(CERR << "Invalid Prices in market data! Symbol [" << mtick.GetSymbol()
				<<"] : Bid [" << mtick.GetBid()
				<< "] Ask [" << mtick.GetAsk() << "]"
				<< std::endl);
		return false;
	}
	return true;
}

bool BidOfferSpread::IsMarketSpreadChanged(StratParams* pStratParams, MTICK& mtick_quote, MTICK& mtick_hedge, bool bBidSpread)
{
	if (bBidSpread)
	{
		/// Check of BidSpread
		if(!(fabs(pStratParams->dContraPricePrevBidHedge - mtick_hedge.GetBid()) > g_dEpsilon))
		{
			// There is no change on Hedge Bid
			if (pStratParams->eStratMode == STRAT_MODE_ALWAYS)
			{
				_STRAT_LOG_DBUG_(CDEBUG << "StratID [" << pStratParams->nStratId
						<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
						<<"] : No update on market bid for hedging contract. PrevBid [" << pStratParams->dContraPricePrevBidHedge
						<< "] = CurrentBid [" << mtick_hedge.GetBid()
						<< "] and ignore quote contract update!"
						<< std::endl);
				return false;
			}
			else if (pStratParams->eStratMode == STRAT_MODE_CONDITIONAL
					&& !(fabs(pStratParams->dContraPricePrevBidQuote - mtick_quote.GetBid()) > g_dEpsilon))
			{
				// MtkBidSpread = Bid_Quote - Bid_Hedge
				_STRAT_LOG_DBUG_(CDEBUG << "StratID [" << pStratParams->nStratId
						<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
						<<"] : No update on market bid spread. "
						<<" - PrevQuoteBid [" << pStratParams->dContraPricePrevBidQuote
						<< "] = CurrentQuoteBid [" << mtick_quote.GetBid()
						<< "] PrevHedgeBid [" << pStratParams->dContraPricePrevBidHedge
						<< "] = CurrentHedgeBid [" << mtick_hedge.GetBid()
						<< "]."
						<< std::endl);
				return false;
			}
			else if (pStratParams->eStratMode == STRAT_MODE_AGGRESSIVE
					 && !(fabs(pStratParams->dContraPricePrevOfferQuote - mtick_quote.GetAsk()) > g_dEpsilon)
					 && (pStratParams->nContraSizePrevOfferQuote == mtick_quote.GetAskSize()))
			{
				// MtkBidSpread = Offer_Quote - Bid_Hedge
				_STRAT_LOG_DBUG_(CDEBUG << "StratID [" << pStratParams->nStratId
						<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
						<<"] : No update on market bid spread. "
						<<" - PrevQuoteOffer [" << pStratParams->dContraPricePrevOfferQuote
						<< "] = CurrentQuoteOffer [" << mtick_quote.GetAsk()
						<< "] PrevHedgeBid [" << pStratParams->dContraPricePrevBidHedge
						<< "] = CurrentHedgeBid [" << mtick_hedge.GetBid()
						<< "] PrevQuoteOffer Size [" << pStratParams->nContraSizePrevOfferQuote
						<< "] CurrentQuoteOffer Size [" << mtick_quote.GetAskSize()
						<< "]."
						<< std::endl);
				return false;
			}
		}
	}
	else if(!(fabs(pStratParams->dContraPricePrevOfferHedge - mtick_hedge.GetAsk()) > g_dEpsilon))
	{
		// There is no change on Hedge Offer
		if (pStratParams->eStratMode == STRAT_MODE_ALWAYS)
		{
			_STRAT_LOG_DBUG_(CDEBUG << "StratID [" << pStratParams->nStratId
					<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
					<<"] : No update on market offer for hedging contract. PrevOffer [" << pStratParams->dContraPricePrevOfferHedge
					<< "] = CurrentOffer [" << mtick_hedge.GetAsk()
					<< "] and ignore quote contract update!"
					<< std::endl);
			return false;
		}
		else if (pStratParams->eStratMode == STRAT_MODE_CONDITIONAL
				&& !(fabs(pStratParams->dContraPricePrevOfferQuote - mtick_quote.GetAsk()) > g_dEpsilon))
		{
			// MtkBidSpread = Offer_Quote - Offer_Hedge
			_STRAT_LOG_DBUG_(CDEBUG << "StratID [" << pStratParams->nStratId
					<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
					<<"] : No update on market offer spread. "
					<<" - PrevQuoteOffer [" << pStratParams->dContraPricePrevOfferQuote
					<< "] = CurrentQuoteOffer [" << mtick_quote.GetAsk()
					<< "] PrevHedgeOffer [" << pStratParams->dContraPricePrevOfferHedge
					<< "] = CurrentHedgeOffer [" << mtick_hedge.GetAsk()
					<< "]."
					<< std::endl);
			return false;
		}
		else if (pStratParams->eStratMode == STRAT_MODE_AGGRESSIVE
				&& !(fabs(pStratParams->dContraPricePrevBidQuote - mtick_quote.GetBid()) > g_dEpsilon)
				 && (pStratParams->nContraSizePrevBidQuote == mtick_quote.GetBidSize()))
		{
			// MtkBidSpread = Bid_Quote - Offer_Hedge
			_STRAT_LOG_DBUG_(CDEBUG << "StratID [" << pStratParams->nStratId
					<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
					<<"] : No update on market offer spread. "
					<<" - PrevQuoteBid [" << pStratParams->dContraPricePrevBidQuote
					<< "] = CurrentQuoteBid [" << mtick_quote.GetBid()
					<< "] PrevHedgeOffer [" << pStratParams->dContraPricePrevOfferHedge
					<< "] = CurrentHedgeOffer [" << mtick_hedge.GetAsk()
					<< "] PrevQuoteBid Size [" << pStratParams->nContraSizePrevBidQuote
					<< "] CurrentQuoteBid Size [" << mtick_quote.GetBidSize()
					<< "]."
					<< std::endl);
			return false;
		}
	}
	// Else: there is a change in market spread
	return true;
}


bool BidOfferSpread::IsSpreadHedgeChanged(StratParams* pStratParams, MTICK& mtick_hedge, bool bBidSpread)
{
	if (bBidSpread)
	{
		// Check of BidSpread
		if(!(fabs(pStratParams->dContraPricePrevBidHedge - mtick_hedge.GetBid()) > g_dEpsilon))
		{
			// There is no change on Hedge Bid
			_STRAT_LOG_DBUG_(CDEBUG << "StratID [" << pStratParams->nStratId
					<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
					<<"] : No update on market bid for hedging contract"
					<< " - Bid Price Prev/Curr [" << pStratParams->dContraPricePrevBidHedge
					<< "/" << mtick_hedge.GetBid()
					<< "]."
					<< std::endl);
			return false;
		}
		else
		{
			_STRAT_LOG_DBUG_(CDEBUG << "StratID [" << pStratParams->nStratId
					<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
					<<"] : Updating market bid for hedging contract"
					<< " - Bid Price Prev/Curr [" << pStratParams->dContraPricePrevBidHedge
					<< "/" << mtick_hedge.GetBid()
					<< "] : Proceed to update corresponding snapshot!"
					<< std::endl);

			pStratParams->dContraPricePrevBidHedge = mtick_hedge.GetBid();

			return true;
		}
	}
	else
	{
		// Check for OfferSpread
		if(!(fabs(pStratParams->dContraPricePrevOfferHedge - mtick_hedge.GetAsk()) > g_dEpsilon))
		{
			// There is no change on Hedge Offer
			_STRAT_LOG_DBUG_(CDEBUG << "StratID [" << pStratParams->nStratId
					<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
					<<"] : No update on market offer for hedging contract"
					<< " - Offer Price Prev/Curr [" << pStratParams->dContraPricePrevOfferHedge
					<< "/" << mtick_hedge.GetAsk()
					<< "]."
					<< std::endl);
			return false;
		}
		else
		{
			_STRAT_LOG_DBUG_(CDEBUG << "StratID [" << pStratParams->nStratId
					<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
					<<"] : Updating market offer for hedging contract"
					<< " - Offer Price Prev/Curr [" << pStratParams->dContraPricePrevOfferHedge
					<< "/" << mtick_hedge.GetAsk()
					<< "] : Proceed to update corresponding snapshot!"
					<< std::endl);
			pStratParams->dContraPricePrevOfferHedge = mtick_hedge.GetAsk();

			return true;
		}
	}
	// Else: there is a change in spread hedge contract
	return true;
}

bool BidOfferSpread::IsSpreadQuoteChanged(StratParams* pStratParams, MTICK& mtick_quote, bool bBidSpread)
{
	if (bBidSpread)
	{
		// Check of BidSpread
		if (!(fabs(pStratParams->dContraPricePrevOfferQuote - mtick_quote.GetAsk()) > g_dEpsilon)
				 && (pStratParams->nContraSizePrevOfferQuote == mtick_quote.GetAskSize()))
		{
			// There is no change on Quote Ask
			_STRAT_LOG_DBUG_(CDEBUG << "StratID [" << pStratParams->nStratId
					<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
					<<"] : No update on market ask for quoting contract"
					<<" - Offer Price Prev/Curr [" << pStratParams->dContraPricePrevOfferQuote
					<< "/" << mtick_quote.GetAsk()
					<< "] Offer Size Prev/Curr [" << pStratParams->nContraSizePrevOfferQuote
					<< "/" << mtick_quote.GetAskSize()
					<< "]."
					<< std::endl);

			return false;
		}
		else
		{
			_STRAT_LOG_VERB_(CWARN << "StratID [" << pStratParams->nStratId
					<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
					<<"] : Updating market ask for quoting contract"
					<<" - Offer Price Prev/Curr [" << pStratParams->dContraPricePrevOfferQuote
					<< "/" << mtick_quote.GetAsk()
					<< "] Offer Size Prev/Curr [" << pStratParams->nContraSizePrevOfferQuote
					<< "/" << mtick_quote.GetAskSize()
					<< "] : Proceed to updated corresponding snapshot!"
					<< std::endl);

			pStratParams->dContraPricePrevOfferQuote = mtick_quote.GetAsk();
			pStratParams->nContraSizePrevOfferQuote = mtick_quote.GetAskSize();

			return true;
		}
	}
	else
	{
		// Check for OfferSpread
		if (!(fabs(pStratParams->dContraPricePrevBidQuote - mtick_quote.GetBid()) > g_dEpsilon)
				&& (pStratParams->nContraSizePrevBidQuote == mtick_quote.GetBidSize()))
		{
			// There is no change on Quote Bid
			_STRAT_LOG_DBUG_(CDEBUG << "StratID [" << pStratParams->nStratId
					<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
					<<"] : No update on market bid for quoting contract"
					<<" - Bid Price Prev/Curr [" << pStratParams->dContraPricePrevBidQuote
					<< "/" << mtick_quote.GetBid()
					<< "] Bid Size Prev/Curr [" << pStratParams->nContraSizePrevBidQuote
					<< "/" << mtick_quote.GetBidSize()
					<< "]."
					<< std::endl);

			return false;
		}
		else
		{
			_STRAT_LOG_VERB_(CWARN << "StratID [" << pStratParams->nStratId
					<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
					<<"] : Updating market bid for quoting contract"
					<<" - Bid Price Prev/Curr [" << pStratParams->dContraPricePrevBidQuote
					<< "/" << mtick_quote.GetBid()
					<< "] Bid Size Prev/Curr [" << pStratParams->nContraSizePrevBidQuote
					<< "/" << mtick_quote.GetBidSize()
					<< "] : Proceed to updated corresponding snapshot!"
					<< std::endl);

			pStratParams->dContraPricePrevBidQuote = mtick_quote.GetBid();
			pStratParams->nContraSizePrevBidQuote = mtick_quote.GetBidSize();

			return true;
		}
	}
}

bool BidOfferSpread::IsSpreadBenchSatisfied(StratParams* pStratParams, MTICK& mtick_quote, MTICK& mtick_hedge, bool bBidSpread)
{
	if (!pStratParams)
	{
		CERR << "Invalid pStratParams pointer" << std::endl;
		return false;
	}

	double dMktBidSpread=0.0, dBidBench=0.0;
	double dMktOfferSpread=0.0, dOfferBench=0.0;
	bool bIsBenchSatisfied = false;
	if (pStratParams->eStratMode == STRAT_MODE_ALWAYS)
	{
		_STRAT_LOG_DBUG_(CDEBUG << "StratId [" << pStratParams->nStratId
								<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
								<< "] : Do not check for market spread!"
								<< std::endl);
		return true;
	}
	else if (pStratParams->eStratMode == STRAT_MODE_CONDITIONAL)
	{
		// Mkt Bid Spread = Bid_Quote - Bid_Hedge
		// Mkt Offer Spread = Offer_Quote - Offer_Hedge
		if (bBidSpread)
		{
			dMktBidSpread = mtick_quote.GetBid() - mtick_hedge.GetBid();
			dBidBench = GetBiddingBenchmark(pStratParams);
			bIsBenchSatisfied = ((dBidBench - dMktBidSpread) > (-g_dEpsilon)) ? true : false;

			_STRAT_LOG_VERB_(CWARN << "StratId [" << pStratParams->nStratId
									<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
									<< "] " << (bBidSpread?"Bid":"Offer")
									<< " Bench [" << dBidBench
									<< "] vs Spread [" << dMktBidSpread
									<< "] : QuoteBid/HedgeBid [" << mtick_quote.GetBid()
									<< "/" << mtick_hedge.GetBid()
									<< "] -> IsBenchSatisfied? [" << bIsBenchSatisfied
									<< "]"
									<<std::endl);
		}
		else
		{
			dMktOfferSpread =  mtick_quote.GetAsk() - mtick_hedge.GetAsk();
			dOfferBench = GetOfferingBenchmark(pStratParams);
			bIsBenchSatisfied = ((dMktOfferSpread - dOfferBench) > (-g_dEpsilon)) ? true : false;

			_STRAT_LOG_VERB_(CWARN << "StratId [" << pStratParams->nStratId
									<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
									<< "] " << (bBidSpread?"Bid":"Offer")
									<< " Bench [" << dOfferBench
									<< "] vs Spread [" << dMktOfferSpread
									<< "] : QuoteOffer/HedgeOffer [" << mtick_quote.GetAsk()
									<< "/" << mtick_hedge.GetAsk()
									<< "] -> IsBenchSatisfied? [" << bIsBenchSatisfied
									<< "]"
									<<std::endl);
		}
		return bIsBenchSatisfied;
	}
	else if (pStratParams->eStratMode == STRAT_MODE_AGGRESSIVE)
	{
		// Mkt Bid Spread = Offer_Quote - Bid_Hedge
		// Mkt Offer Spread = Bid_Quote - Offer_Hedge
		if (bBidSpread)
		{
			dMktBidSpread = mtick_quote.GetAsk() - mtick_hedge.GetBid();
			dBidBench = GetBiddingBenchmark(pStratParams);
			bIsBenchSatisfied = ((dBidBench - dMktBidSpread) > (-g_dEpsilon)) ? true : false;

			_STRAT_LOG_VERB_(CWARN << "StratId [" << pStratParams->nStratId
									<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
									<< "] " << (bBidSpread?"Bid":"Offer")
									<< " Bench [" << dBidBench
									<< "] vs Spread [" << dMktBidSpread
									<< "] : QuoteOffer/HedgeBid [" << mtick_quote.GetAsk()
									<< "/" << mtick_hedge.GetBid()
									<< "] -> IsBenchSatisfied? [" << bIsBenchSatisfied
									<< "]"
									<<std::endl);
		}
		else
		{
			dMktOfferSpread =  mtick_quote.GetBid() - mtick_hedge.GetAsk();
			dOfferBench = GetOfferingBenchmark(pStratParams);
			bIsBenchSatisfied = ((dMktOfferSpread - dOfferBench) > (-g_dEpsilon)) ? true : false;

			_STRAT_LOG_VERB_(CWARN << "StratId [" << pStratParams->nStratId
									<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
									<< "] " << (bBidSpread?"Bid":"Offer")
									<< " Bench [" << dOfferBench
									<< "] vs Spread [" << dMktOfferSpread
									<< "] : QuoteBid/HedgeOffer [" << mtick_quote.GetBid()
									<< "/" << mtick_hedge.GetAsk()
									<< "] -> IsBenchSatisfied? [" << bIsBenchSatisfied
									<< "]"
									<<std::endl);
		}
		return bIsBenchSatisfied;
	}
	else
	{
		CERR << "StratID [" << pStratParams-> nStratId
				<< "] INVALID QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
				<< "]!"
				<< std::endl;
		return false;
	}
}

bool BidOfferSpread::UpdateStageBenchmarks(StratParams* pStratParams)
{
	CLIENT_ORDER_CONTAINER COrdContainer;
	CLIENT_ORDER BidQuoteCientOrder, OfferQuoteClientOrder;
	if (!COrdContainer.GetByClientOrderId(BidQuoteCientOrder, pStratParams->szClientOrdIdBidQuote)
			|| !COrdContainer.GetByClientOrderId(OfferQuoteClientOrder, pStratParams->szClientOrdIdOfferQuote))
			{
				CERR << "StratID [" << pStratParams-> nStratId
						<< "] Failed to get quote client orders!"
						<< "]"
						<< std::endl;
				return false;
			}

	int nNetPos=0, nStageInterval=1, nBidStage=0, nOfferStage=0, nStages=1, nMaxPos=0, nSpdInt;
	double dticksize_quote = (g_dEpsilon/10.0);
	nStages = pStratParams->nNumStages;
	nMaxPos = pStratParams->nMaxPosition;
	nSpdInt = pStratParams->nSpreadInterval;
	nStageInterval =  (nMaxPos%nStages > 0)? int(nMaxPos/nStages)+1 : int(nMaxPos/nStages);
	nNetPos = BidQuoteCientOrder.GetStreetExecs() - OfferQuoteClientOrder.GetStreetExecs();

	dticksize_quote = GetTickSizeForSymbol(pStratParams->szSymbolQuote);
	if (dticksize_quote < g_dEpsilon)
	{
		CERR << "Invalide Tick Size [" << dticksize_quote
				<<"] for Symbol [" << pStratParams->szSymbolQuote
				<<"]"
				<<std::endl;
		return false;
	}

	nBidStage = nNetPos >= 0 ? int((abs(nNetPos))/nStageInterval) : 0;
	nOfferStage = nNetPos >= 0 ? 0 : int((abs(nNetPos))/nStageInterval);

	pStratParams->dStateBidBench = pStratParams->dBenchmarkBid - dticksize_quote * nBidStage * nSpdInt;
	pStratParams->dStateOfferBench = pStratParams->dBenchmarkOffer + dticksize_quote * nOfferStage * nSpdInt;

	_STRAT_LOG_VERB_(CWARN << "Updated Stage Spreads :  Symbol [" <<  pStratParams->szSymbolQuote
					<<"] BotQty [" << BidQuoteCientOrder.GetStreetExecs()
					<<"] SoldQty [" << OfferQuoteClientOrder.GetStreetExecs()
					<<"] NetPos [" << nNetPos
					<<"] SpreadInterval [" << nSpdInt
					<<"] TickSIze [" << dticksize_quote
					<<"]\n -> Bidding Stage [" << nBidStage
					<<"] StageSpread [" << pStratParams->dStateBidBench
					<<"] : Bid BenchSpread [" << pStratParams->dBenchmarkBid
					<<"] reduced by StageSpread (SpdInt*Stage) Ticks [" << nBidStage * nSpdInt
					<<"]\n -> Offering Stage[" << nOfferStage
					<<"] StageSpread [" << pStratParams->dStateOfferBench
					<<"] : Ofer BenchSpread [" << pStratParams->dBenchmarkOffer
					<<"] increased by StageSpread (SpdInt*Stage) Ticks [" << nOfferStage * nSpdInt
					<<"] "
					<< std::endl);

	return true;
}

double	BidOfferSpread::GetBiddingBenchmark(StratParams* pStratParams)
{
	UpdateStageBenchmarks(pStratParams);
	return pStratParams->dStateBidBench;
}

double	BidOfferSpread::GetOfferingBenchmark(StratParams* pStratParams)
{
	UpdateStageBenchmarks(pStratParams);
	return pStratParams->dStateOfferBench;
}

int	BidOfferSpread::CalculateNetStratExecs(StratParams* pStratParams)
{
	// STEP 1 : Get all client order
	CLIENT_ORDER BidQuoteOrder, BidHedgeOrder, OfferQuoteOrder, OfferHedgeOrder;
	GetQuoteClientOrder(BidQuoteOrder, pStratParams, true);
	GetQuoteClientOrder(OfferQuoteOrder, pStratParams, false);
	GetHedgeClientOrder(BidHedgeOrder, pStratParams, true);
	GetHedgeClientOrder(OfferHedgeOrder, pStratParams, false);

	// STEP 2 : Calculate current NetStratExecs
	int nNetStratExecs = pStratParams->nQtyPerOrder + 1;
	nNetStratExecs = BidQuoteOrder.GetStreetExecs() - OfferQuoteOrder.GetStreetExecs()
						+ BidHedgeOrder.GetStreetExecs() - OfferHedgeOrder.GetStreetExecs();

	return nNetStratExecs;
}

int BidOfferSpread::ValidateFIXTags(ORDER& order)
{
	std::string sFixDestination = order.GetFixTag(FIX_TAG_DESTINATION);
	if(sFixDestination.empty())
	{
		std::stringstream errMsg;
		errMsg << "Please set destination in tag [" << FIX_TAG_DESTINATION << "]";
		CERR << "Order Validation Failed, cannot send to destination " << sFixDestination
				<< " , rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

    std::string szOMUser = order.GetFixTag(FIX_TAG_OMUSER);
	if(szOMUser.length() <= 0 )
	{
		std::stringstream errMsg;
		errMsg << "Please set OMUser in tag [" << FIX_TAG_OMUSER << "]";
		CERR << "Order Validation Failed, invalid OM User, rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	std::string sFixQuotingMode = order.GetFixTag(FIX_TAG_QUOTEMODE_BO);
	if(sFixQuotingMode.empty())
	{
		std::stringstream errMsg;
		errMsg << "Please set QuotingMode in tag [" << FIX_TAG_QUOTEMODE_BO << "]";
		CERR << "Order Validation Failed, cannot determine quoting mode " << sFixQuotingMode
				<< " , rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	int nQuotingMode = atoi(order.GetFixTag(FIX_TAG_QUOTEMODE_BO));
	if(nQuotingMode < MODEID_MIN_BO || nQuotingMode > MODEID_MAX_BO )
	{
		std::stringstream errMsg;
		errMsg << "Invalid QuoteModeId in tag [" << FIX_TAG_QUOTEMODE_BO <<"]";
		CERR << "Order Validation Failed, invalid QuoteModeId [" << nQuotingMode << "], rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	std::string sStratId = order.GetFixTag(FIX_TAG_STRATID_BO);
	if(sStratId.empty())
	{
		std::stringstream errMsg;
		errMsg << "Please set Strat Id in tag [" << FIX_TAG_STRATID_BO << "]";
		CERR << "Order Validation Failed, Strat Id is not set , rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	long nStratId = atol(order.GetFixTag(FIX_TAG_STRATID_BO));
	if(gmStratMap.count(nStratId) >= 2 )
	{
		std::stringstream errMsg;
		errMsg << "Invalid StratId in tag [" << FIX_TAG_STRATID_BO << "]";
		CERR << "Order Validation Failed, duplicate Strat Id [" << nStratId
				<< "], rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	std::string sStratPort = order.GetFixTag(FIX_TAG_STRATPORT_BO);
	if(sStratPort.empty())
	{
		std::stringstream errMsg;
		errMsg << "Please set StratPort in tag [" << FIX_TAG_STRATPORT_BO << "]";
		CERR << "Order Validation Failed, StratPort is not set , rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	std::string sBenchmark = order.GetFixTag(FIX_TAG_BENCHMARK_BO);
	if(sBenchmark.empty())
	{
		std::stringstream errMsg;
		errMsg << "Please set Benchmark in tag [" << FIX_TAG_BENCHMARK_BO << "]";
		CERR << "Order Validation Failed, Benchmark is not set , rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	int nStratRunning = atoi(order.GetFixTag(FIX_TAG_RUNNING_BO));
	if(!(nStratRunning==0 || nStratRunning ==1))
	{
		std::stringstream errMsg;
		errMsg << "Invalid Running Flag in tag [" << FIX_TAG_RUNNING_BO <<"]";
		CERR << "Order Validation Failed, invalid strat playpause tag [" << nStratRunning << "], rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	std::string sOrdQty = order.GetFixTag(FIX_TAG_ORDQTY_BO);
	if(sOrdQty.empty())
	{
		std::stringstream errMsg;
		errMsg << "Please set OrdQty in tag [" << FIX_TAG_ORDQTY_BO << "]";
		CERR << "Order Validation Failed, Ord Qty is not set , rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	int nOrderQty = atoi(order.GetFixTag(FIX_TAG_ORDQTY_BO));
	if(nOrderQty <= 0)
	{
		std::stringstream errMsg;
		errMsg << "Invalid OrdQty in tag [" << FIX_TAG_ORDQTY_BO <<"]";
		CERR << "Order Validation Failed, invalid order quantity [" << nOrderQty << "], rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	std::string sMaxPos = order.GetFixTag(FIX_TAG_MAXPOS_BO);
	if(sMaxPos.empty())
	{
		std::stringstream errMsg;
		errMsg << "Please set Max Pos in tag [" << FIX_TAG_MAXPOS_BO << "]";
		CERR << "Order Validation Failed, Max Pos is not set , rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	int nMaxPos = atoi(order.GetFixTag(FIX_TAG_MAXPOS_BO));
	if(nMaxPos <= 0)
	{
		std::stringstream errMsg;
		errMsg << "Invalid MaxPos in tag [" << FIX_TAG_MAXPOS_BO <<"]";
		CERR << "Order Validation Failed, invalid Max Pos [" << nMaxPos << "], rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	std::string sLotSize = order.GetFixTag(FIX_TAG_LOTSIZE_BO);
	if(sLotSize.empty())
	{
		std::stringstream errMsg;
		errMsg << "Please set Lot Size in tag [" << FIX_TAG_LOTSIZE_BO << "]";
		CERR << "Order Validation Failed, Lot Size is not set , rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	int nLotSize = atoi(order.GetFixTag(FIX_TAG_LOTSIZE_BO));
	if(nLotSize <= 0)
	{
		std::stringstream errMsg;
		errMsg << "Invalid LotSize in tag [" << FIX_TAG_LOTSIZE_BO <<"]";
		CERR << "Order Validation Failed, invalid Lot Size [" << nLotSize << "], rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	std::string sLegId = order.GetFixTag(FIX_TAG_LEGID_BO);
	if(sLegId.empty())
	{
		std::stringstream errMsg;
		errMsg << "Please set Leg Id in tag [" << FIX_TAG_LEGID_BO << "]";
		CERR << "Order Validation Failed, Leg Id is not set , rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	int nLegId = atoi(order.GetFixTag(FIX_TAG_LEGID_BO));
	if(nLegId < LEGID_MIN_BO || nLegId > LEGID_MAX_BO )
	{
		std::stringstream errMsg;
		errMsg << "Invalid LegId in tag [" << FIX_TAG_LEGID_BO <<"]";
		CERR << "Order Validation Failed, invalid Leg Id [" << nLegId << "], rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	std::string sPayUpTick = order.GetFixTag(FIX_TAG_PAYUPTICK_BO);
	if(sPayUpTick.empty())
	{
		std::stringstream errMsg;
		errMsg << "Please set PayUp Tick in tag [" << FIX_TAG_PAYUPTICK_BO << "]";
		CERR << "Order Validation Failed, Payup Tick is not set , rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	int nPayUpTick = atoi(order.GetFixTag(FIX_TAG_PAYUPTICK_BO));
	if(nPayUpTick < 0)
	{
		std::stringstream errMsg;
		errMsg << "Invalid PayUp Tick in tag [" << FIX_TAG_LOTSIZE_BO <<"]";
		CERR << "Order Validation Failed, invalid PayUp Tick [" << nPayUpTick << "], rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	std::string sMaxRpl = order.GetFixTag(FIX_TAG_MAXRPL_BO);
	if(sMaxRpl.empty())
	{
		std::stringstream errMsg;
		errMsg << "Please set Max Rpl in tag [" << FIX_TAG_MAXRPL_BO << "]";
		CERR << "Order Validation Failed, Max Rpl is not set , rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	int nMaxRpl = atoi(order.GetFixTag(FIX_TAG_MAXRPL_BO));
	if(nMaxRpl < 0)
	{
		std::stringstream errMsg;
		errMsg << "Invalid Max Rpl in tag [" << FIX_TAG_MAXRPL_BO <<"]";
		CERR << "Order Validation Failed, invalid Max Rpl [" << nMaxRpl << "], rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	std::string sMaxLoss = order.GetFixTag(FIX_TAG_MAXLOSS_BO);
	if(sMaxLoss.empty())
	{
		std::stringstream errMsg;
		errMsg << "Please set Max Loss in tag [" << FIX_TAG_MAXLOSS_BO << "]";
		CERR << "Order Validation Failed, Max Loss is not set , rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	double dMaxLoss = atoi(order.GetFixTag(FIX_TAG_MAXLOSS_BO));
	if(dMaxLoss < -g_dEpsilon)
	{
		std::stringstream errMsg;
		errMsg << "Invalid Max Loss in tag [" << FIX_TAG_MAXLOSS_BO <<"]";
		CERR << "Order Validation Failed, invalid Max Loss [" << dMaxLoss << "], rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	std::string sMaxStage = order.GetFixTag(FIX_TAG_STGMAXCOUNT_BO);
	if(sMaxStage.empty())
	{
		std::stringstream errMsg;
		errMsg << "Please set Max Stage Count in tag [" << FIX_TAG_STGMAXCOUNT_BO << "]";
		CERR << "Order Validation Failed, Max Stage Count is not set , rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	int nMaxStage = atoi(order.GetFixTag(FIX_TAG_STGMAXCOUNT_BO));
	if(nMaxStage < 0)
	{
		std::stringstream errMsg;
		errMsg << "Invalid Max Stage Count in tag [" << FIX_TAG_STGMAXCOUNT_BO <<"]";
		CERR << "Order Validation Failed, invalid Max Stage Count [" << nMaxStage << "], rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	std::string sSpreadInterval = order.GetFixTag(FIX_TAG_STGSPDINTERVAL_BO);
	if(sSpreadInterval.empty())
	{
		std::stringstream errMsg;
		errMsg << "Please set Spread Interval in tag [" << FIX_TAG_STGSPDINTERVAL_BO << "]";
		CERR << "Order Validation Failed, Spread Interval is not set , rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	int nSpreadInterval = atoi(order.GetFixTag(FIX_TAG_STGSPDINTERVAL_BO));
	if(nSpreadInterval < 0)
	{
		std::stringstream errMsg;
		errMsg << "Invalid Spread Interval in tag [" << FIX_TAG_STGSPDINTERVAL_BO <<"]";
		CERR << "Order Validation Failed, invalid Spread Interval [" << nSpreadInterval << "], rejecting client order!!! " << errMsg.str() << std::endl;
		order.SetRejectMsg(errMsg.str());
		return FAILURE;
	}

	return SUCCESS;
}

int BidOfferSpread::AddStratParamsToMap(ORDER& order, STRATMAP& mStratMap )
{
	//Populate Structure and add to map
	long nStratId = atol(order.GetFixTag(FIX_TAG_STRATID_BO));
	int nLegId = atoi(order.GetFixTag(FIX_TAG_LEGID_BO));

	_STRAT_LOG_DBUG_(CDEBUG << "Adding OrderID [" << order.GetHandle()
							<< "] in StratMap! : With StratID [" <<  nStratId
							<< "] LegID [" << nLegId
							<< "]"
							<<  std::endl);

	if(mStratMap.count(nStratId) <= 0)
	{
		//First Entry into Strat
		_STRAT_LOG_DBUG_(CDEBUG << "StratID [" << nStratId << "] : First entry to mStratMap!" <<  std::endl);

		StratParams *pStratParams = new StratParams();
		if(!pStratParams)
			return FAILURE;

		pStratParams->nStratId = atol(order.GetFixTag(FIX_TAG_STRATID_BO));
		strcpy(pStratParams->szStratPort,order.GetFixTag(FIX_TAG_STRATPORT_BO));

		// LEGID_BO_BQ : Bid Quote
		if(nLegId == LEGID_BO_BQ)
		{
			_STRAT_LOG_DBUG_(CDEBUG << "Adding OrderID [" << order.GetHandle()
										<< "] in BidQuote! : With StratID [" <<  nStratId
										<< "] LegID [" << nLegId
										<< "]"
										<<  std::endl);

			strcpy(pStratParams->szSymbolQuote,order.GetSymbol());
			strcpy(pStratParams->szClientOrdIdBidQuote,order.GetHandle());
			strcpy(pStratParams->szPortfolioBid,order.GetPortfolio());
			pStratParams->dBenchmarkBid = atof(order.GetFixTag(FIX_TAG_BENCHMARK_BO));
			pStratParams->nLotSizeQuote=atoi(order.GetFixTag(FIX_TAG_LOTSIZE_BO));
			pStratParams->enSideBidQuote = order.GetSide();
			pStratParams->nQtyPerOrder = atoi(order.GetFixTag(FIX_TAG_ORDQTY_BO));
			pStratParams->nMaxPosition = atoi(order.GetFixTag(FIX_TAG_MAXPOS_BO));
			pStratParams->nPayUpTicks = atoi(order.GetFixTag(FIX_TAG_PAYUPTICK_BO));
			pStratParams->nMaxRplLimit = atoi(order.GetFixTag(FIX_TAG_MAXRPL_BO));
			pStratParams->dMaxLoss = atof(order.GetFixTag(FIX_TAG_MAXLOSS_BO));
			pStratParams->nNumStages = atoi(order.GetFixTag(FIX_TAG_STGMAXCOUNT_BO));
			pStratParams->nSpreadInterval = atoi(order.GetFixTag(FIX_TAG_STGSPDINTERVAL_BO));

			switch (atoi(order.GetFixTag(FIX_TAG_QUOTEMODE_BO)))
			{
				case  MODEID_ALWAYS :
					pStratParams->eStratMode = STRAT_MODE_ALWAYS;
					break;
				case MODEID_CONDITIONAL :
					pStratParams->eStratMode = STRAT_MODE_CONDITIONAL;
					break;
				case MODEID_AGGRESSIVE :
					pStratParams->eStratMode = STRAT_MODE_AGGRESSIVE;
					break;
				default :
					pStratParams->eStratMode = STRAT_MODE_INVALID;
			}

			pStratParams->bBidQuoteEntry = true;

			_STRAT_LOG_DBUG_(CDEBUG << "StratID [" << nStratId
										<< "] in BidQuote! : szClientOrdIdBidQuote=[" <<  pStratParams->szClientOrdIdBidQuote
										<< "] szClientOrdIdOfferQuote=[" << pStratParams->szClientOrdIdOfferQuote
										<< "] szClientOrdIdBidHedge=[" << pStratParams->szClientOrdIdBidHedge
										<< "] szClientOrdIdOfferHedge=[" << pStratParams->szClientOrdIdOfferHedge
										<< "]"
										<<  std::endl);
		}
		// LEGID_BO_BH : Bid Hedge
		else if(nLegId == LEGID_BO_BH)
		{
			_STRAT_LOG_DBUG_(CDEBUG << "Adding OrderID [" << order.GetHandle()
										<< "] in BidHedge! : With StratID [" <<  nStratId
										<< "] LegID [" << nLegId
										<< "]"
										<<  std::endl);

			strcpy(pStratParams->szSymbolHedge,order.GetSymbol());
			strcpy(pStratParams->szClientOrdIdBidHedge,order.GetHandle());

			pStratParams->nLotSizeHedge=atoi(order.GetFixTag(FIX_TAG_LOTSIZE_BO));
			pStratParams->enSideBidHedge = order.GetSide();

			pStratParams->bBidHedgeEntry = true;
		}
		// LEGID_BO_OQ : Offer Quote
		else if(nLegId == LEGID_BO_OQ)
		{
			_STRAT_LOG_DBUG_(CDEBUG << "Adding OrderID [" << order.GetHandle()
										<< "] in OfferQuote! : With StratID [" <<  nStratId
										<< "] LegID [" << nLegId
										<< "]"
										<<  std::endl);

			strcpy(pStratParams->szClientOrdIdOfferQuote,order.GetHandle());
			strcpy(pStratParams->szPortfolioOffer,order.GetPortfolio());
			pStratParams->dBenchmarkOffer = atof(order.GetFixTag(FIX_TAG_BENCHMARK_BO));

			pStratParams->enSideOfferQuote = order.GetSide();

			pStratParams->bOfferQuoteEntry = true;
		}
		// LEGID_BO_OH : Offer Hedge
		else if(nLegId == LEGID_BO_OH)
		{
			_STRAT_LOG_DBUG_(CDEBUG << "Adding OrderID [" << order.GetHandle()
										<< "] in OfferHedge! : With StratID [" <<  nStratId
										<< "] LegID [" << nLegId
										<< "]"
										<<  std::endl);

			strcpy(pStratParams->szClientOrdIdOfferHedge,order.GetHandle());

			pStratParams->enSideOfferHedge = order.GetSide();

			pStratParams->bOfferHedgeEntry = true;
		}

		strcpy(pStratParams->szOMUser,order.GetFixTag(FIX_TAG_OMUSER));
		strcpy(pStratParams->szDest,order.GetFixTag(FIX_TAG_DESTINATION));

		//Optimisation to save fix tag string construction on market data
		pStratParams->bStratRunning=(bool)atoi(order.GetFixTag(FIX_TAG_RUNNING_BO));

		mStratMap.insert(pair<int,StratParams*>(nStratId,pStratParams));

		_STRAT_LOG_DBUG_(PrintStratOrderMap(gmStratMap, nStratId));
	}
	else
	{
		//Following Entry into Strat
		_STRAT_LOG_DBUG_(CDEBUG << "StratID [" << nStratId << "] : Subsequent entry to mStratMap!" << std::endl);
		STRATMAPITER mStIter = mStratMap.find(nStratId);

		if (mStIter == mStratMap.end())
		{

			CERR << "StratId [" << nStratId
					<< "] has count [" << mStratMap.count(nStratId)
					<< "] but failed to find in gmStratMap!!!" << std::endl;

			return FAILURE;
		}

		StratParams *pStratParams = (StratParams*) mStIter->second;

		if(!pStratParams)
			return FAILURE;

		// LEGID_BO_BQ : Bid Quote
		if(nLegId == LEGID_BO_BQ)
		{
			strcpy(pStratParams->szSymbolQuote,order.GetSymbol());
			strcpy(pStratParams->szClientOrdIdBidQuote,order.GetHandle());
			strcpy(pStratParams->szPortfolioBid,order.GetPortfolio());
			pStratParams->dBenchmarkBid = atof(order.GetFixTag(FIX_TAG_BENCHMARK_BO));
			pStratParams->nLotSizeQuote=atoi(order.GetFixTag(FIX_TAG_LOTSIZE_BO));
			pStratParams->enSideBidQuote = order.GetSide();

			pStratParams->bBidQuoteEntry = true;

			_STRAT_LOG_DBUG_(PrintStratOrderMap(mStratMap, nStratId));
		}
		// LEGID_BO_BH : Bid Hedge
		if(nLegId == LEGID_BO_BH)
		{
			strcpy(pStratParams->szSymbolHedge,order.GetSymbol());
			strcpy(pStratParams->szClientOrdIdBidHedge,order.GetHandle());

			pStratParams->nLotSizeHedge=atoi(order.GetFixTag(FIX_TAG_LOTSIZE_BO));
			pStratParams->enSideBidHedge = order.GetSide();

			pStratParams->bBidHedgeEntry = true;

			_STRAT_LOG_DBUG_(PrintStratOrderMap(mStratMap, nStratId));
		}
		// LEGID_BO_OQ : Offer Quote
		if(nLegId == LEGID_BO_OQ)
		{

			strcpy(pStratParams->szClientOrdIdOfferQuote,order.GetHandle());
			strcpy(pStratParams->szPortfolioOffer,order.GetPortfolio());
			pStratParams->dBenchmarkOffer = atof(order.GetFixTag(FIX_TAG_BENCHMARK_BO));

			pStratParams->enSideOfferQuote = order.GetSide();

			pStratParams->bOfferQuoteEntry = true;

			_STRAT_LOG_DBUG_(PrintStratOrderMap(mStratMap, nStratId));
		}
		// LEGID_BO_OH : Offer Hedge
		if(nLegId == LEGID_BO_OH)
		{
			strcpy(pStratParams->szClientOrdIdOfferHedge,order.GetHandle());

			pStratParams->enSideOfferHedge = order.GetSide();

			pStratParams->bOfferHedgeEntry = true;

			_STRAT_LOG_DBUG_(PrintStratOrderMap(mStratMap, nStratId));
		}

		if (pStratParams->bBidQuoteEntry && pStratParams->bBidHedgeEntry
			&& pStratParams->bOfferQuoteEntry && pStratParams->bOfferHedgeEntry)
		{
			_STRAT_LOG_DBUG_(CDEBUG << "StratID [" <<  pStratParams->nStratId
									<< "] is ready in StratParamMap!"
									<< std::endl);

			pStratParams->bStratReady = true;

			PrintStratOrderMap(mStratMap, nStratId);

			PublishStratStatus(pStratParams);
		}
	}
	return SUCCESS;
}

int BidOfferSpread::GetQuoteClientOrder(CLIENT_ORDER& QuoteOrder, StratParams* pStratParams, bool bBidSpread)
{
	if (bBidSpread)
	{
		if(!GetClientOrderById(QuoteOrder, pStratParams->szClientOrdIdBidQuote))
		{
			CERR << "StratID [" << pStratParams->nStratId
					<< "] : Failed to get bidding quote order by ID [ " << pStratParams->szClientOrdIdBidQuote
					<< "]"
					<< std::endl;
			return FAILURE;
		}
	}
	else
	{
		if(!GetClientOrderById(QuoteOrder, pStratParams->szClientOrdIdOfferQuote))
		{
			CERR << "StratID [" << pStratParams->nStratId
					<< "] : Failed to get offering quote order by ID [ " << pStratParams->szClientOrdIdOfferQuote
					<< "]"
					<< std::endl;
			return FAILURE;
		}
	}
	return SUCCESS;
}

int BidOfferSpread::GetHedgeClientOrder(CLIENT_ORDER& HedgeOrder, StratParams* pStratParams, bool bBidSpread)
{
	if (bBidSpread)
	{
		if(!GetClientOrderById(HedgeOrder, pStratParams->szClientOrdIdBidHedge))
		{
			CERR << "StratID [" << pStratParams->nStratId
					<< "] : Failed to get bidding hedge order by ID [ " << pStratParams->szClientOrdIdBidHedge
					<< "]"
					<< std::endl;
			return FAILURE;
		}
	}
	else
	{
		if(!GetClientOrderById(HedgeOrder, pStratParams->szClientOrdIdOfferHedge))
		{
			CERR << "StratID [" << pStratParams->nStratId
					<< "] : Failed to get offering hedge order by ID [ " << pStratParams->szClientOrdIdOfferHedge
					<< "]"
					<< std::endl;
			return FAILURE;
		}
	}
	return SUCCESS;
}

// Return: TRUE - if adjustment is completed; FALSE - if adjustment is in progress
bool BidOfferSpread::IsHedgeAdjustComplete (StratParams* pStratParams, int nNetStratExecs)
{
	CLIENT_ORDER BidHedgeOrder, OfferHedgeOrder;
	GetHedgeClientOrder(BidHedgeOrder, pStratParams, true);
	GetHedgeClientOrder(OfferHedgeOrder, pStratParams, false);

	bool bHedgeAdjustComplete = false;
	if (nNetStratExecs == 0)
	{
		// Strat is hedged - There should be no working orders on hedge contract
		bHedgeAdjustComplete = (BidHedgeOrder.GetWorkingSize() == 0) && (OfferHedgeOrder.GetWorkingSize() == 0);

	}
	else if (nNetStratExecs > 0)
	{
		// Strat is net long
		bHedgeAdjustComplete = (BidHedgeOrder.GetWorkingSize() == 0) && (OfferHedgeOrder.GetWorkingSize() == nNetStratExecs);
	}
	else
	{
		// Strat is net short
		bHedgeAdjustComplete = (OfferHedgeOrder.GetWorkingSize() == 0) && (BidHedgeOrder.GetWorkingSize() == abs(nNetStratExecs));
	}

	if (bHedgeAdjustComplete)
	{
		_STRAT_LOG_VERB_(CWARN << "Strat ID [" << pStratParams-> nStratId
									<< "] NetStratExecs [" << nNetStratExecs
									<< "] - BidHedgeOrder WorkingSize [" << BidHedgeOrder.GetWorkingSize()
									<< "] OfferHedgeOrder WorkingSize [" << OfferHedgeOrder.GetWorkingSize()
									<< "] : Hedge Adjusted!"
									<<	std::endl);
	}
	else
	{
		_STRAT_LOG_VERB_(CWARN << "Strat ID [" << pStratParams-> nStratId
									<< "] NetStratExecs [" << nNetStratExecs
									<< "] - BidHedgeOrder WorkingSize [" << BidHedgeOrder.GetWorkingSize()
									<< "] OfferHedgeOrder WorkingSize [" << OfferHedgeOrder.GetWorkingSize()
									<< "] : Hedge NOT Adjusted!"
									<<	std::endl);
	}

	return bHedgeAdjustComplete;
}

// Adjust working size on hedge contract
// If called from OnMD, dDriverLastPrice = 0.0, nDriverLastQty = 0
int BidOfferSpread::HandleHedgeAdjustWorkingSize (StratParams* pStratParams, int nNetStratExecs, double dDriverLastPrice, int nDriverLastQty, int nDriverLastLegId)
{
	CLIENT_ORDER BidHedgeOrder, OfferHedgeOrder;
	GetHedgeClientOrder(BidHedgeOrder, pStratParams, true);
	GetHedgeClientOrder(OfferHedgeOrder, pStratParams, false);

	int nTargetPos = 0;
	nTargetPos = abs(nNetStratExecs);

	if (nNetStratExecs == 0)
	{
		// Strat is hedged - Cancel all orders on hedge contract
		if (BidHedgeOrder.GetWorkingSize() == 0 && OfferHedgeOrder.GetWorkingSize() == 0)
		{
			return SUCCESS;
		}
		else
		{
			if (BidHedgeOrder.GetWorkingSize() != 0)
			{
				CancelAllOrdersForClientOrder(BidHedgeOrder.GetHandle());
			}
			if (OfferHedgeOrder.GetWorkingSize() !=0 )
			{
				CancelAllOrdersForClientOrder(OfferHedgeOrder.GetHandle());
			}
			return SUCCESS;
		}
	}
	else if (nNetStratExecs > 0)
	{
		// Strat is net long
		if (BidHedgeOrder.GetWorkingSize() != 0)
		{
			CancelAllOrdersForClientOrder(BidHedgeOrder.GetHandle());
		}

		if (OfferHedgeOrder.GetWorkingSize() > nTargetPos)
		{
			// Cancel highest offers until working size will be within nNetStratExecs
			return CancelExcessOrderForClientOrder(OfferHedgeOrder.GetHandle(), nTargetPos);
		}
		else if (OfferHedgeOrder.GetWorkingSize() < nTargetPos)
		{
			// Send new hedge
			int nNewHedgeQty = 0;
			double dNewHedgePrice=0.0;

			if (nDriverLastLegId != LEGID_BO_BQ)
			{
				_STRAT_LOG_VERB_(CERR << "StratID [" <<  pStratParams->nStratId
										<< "] NetStratExecs [" << nNetStratExecs
										<< "] OfferHedge WorkingSize [" <<  OfferHedgeOrder.GetWorkingSize()
										<< "] - Invalid DriverLastLegId [" << nDriverLastLegId
										<< "] : Not sending new offer on hedge contract!"
										<< std::endl);
				return FAILURE;
			}
			else if (nDriverLastQty <= 0 || dDriverLastPrice <= g_dEpsilon)
			{
				_STRAT_LOG_DBUG_(CDEBUG << "StratID [" <<  pStratParams->nStratId
									<< "] NetStratExecs [" << nNetStratExecs
									<< "] OfferHedge WorkingSize [" <<  OfferHedgeOrder.GetWorkingSize()
									<< "] - Invalid Bid Quote DriverLastPrice [" <<  dDriverLastPrice
									<< "] or DriverLastQty [" << nDriverLastQty
									<< "] : Called from OnMD!"
									<< std::endl);
				return FAILURE;
			}
			else
			{
				nNewHedgeQty = min (nTargetPos - OfferHedgeOrder.GetWorkingSize(), nDriverLastQty);

				dNewHedgePrice = GetStratHedgePrice(pStratParams, dDriverLastPrice, false);

				_STRAT_LOG_VERB_(CWARN << "StratID [" <<  pStratParams->nStratId
									<< "] NetStratExecs [" << nNetStratExecs
									<< "] OfferHedge WorkingSize [" <<  OfferHedgeOrder.GetWorkingSize()
									<< "] - Bid Quote DriverLastPrice [" <<  dDriverLastPrice
									<< "] DriverLastQty [" << nDriverLastQty
									<< "] : Setting new HedgeOffer Qty = [" << nNewHedgeQty
									<< "] at Price = [" << dNewHedgePrice
									<< "] "
									<< std::endl);
			}

			bool bStreetSent = SendOutStreetOrder(OfferHedgeOrder, pStratParams->szDest, nNewHedgeQty, dNewHedgePrice);

			if (bStreetSent)
				return SUCCESS;
			else
				return FAILURE;
		}
	}
	else
	{
		// Strat is net short
		if (OfferHedgeOrder.GetWorkingSize() != 0)
		{
			CancelAllOrdersForClientOrder(OfferHedgeOrder.GetHandle());
		}

		if (BidHedgeOrder.GetWorkingSize() > nTargetPos)
		{
			// Cancel lowest bids until working size will be within nNetStratExecs
			return CancelExcessOrderForClientOrder(BidHedgeOrder.GetHandle(), nTargetPos);
		}
		else if (BidHedgeOrder.GetWorkingSize() < nTargetPos)
		{
			// Send new hedge
			int nNewHedgeQty = 0;
			double dNewHedgePrice=0.0;

			if (nDriverLastLegId != LEGID_BO_OQ)
			{
				_STRAT_LOG_VERB_(CERR << "StratID [" <<  pStratParams->nStratId
										<< "] NetStratExecs [" << nNetStratExecs
										<< "] BidHedge WorkingSize [" <<  BidHedgeOrder.GetWorkingSize()
										<< "] - Invalid DriverLastLegId [" << nDriverLastLegId
										<< "] : Not sending new bid on hedge contract!"
										<< std::endl);
				return FAILURE;
			}
			else if (nDriverLastQty <= 0 || dDriverLastPrice <= g_dEpsilon)
			{
				_STRAT_LOG_DBUG_(CDEBUG << "StratID [" <<  pStratParams->nStratId
									<< "] NetStratExecs [" << nNetStratExecs
									<< "] BidHedge WorkingSize [" <<  BidHedgeOrder.GetWorkingSize()
									<< "] - Invalid Offer Quote DriverLastPrice [" <<  dDriverLastPrice
									<< "] or DriverLastQty [" << nDriverLastQty
									<< "] "
									<< std::endl);
				return FAILURE;
			}
			else
			{
				nNewHedgeQty = min (nTargetPos - BidHedgeOrder.GetWorkingSize(), nDriverLastQty);

				dNewHedgePrice = GetStratHedgePrice(pStratParams, dDriverLastPrice, true);

				_STRAT_LOG_VERB_(CWARN << "StratID [" <<  pStratParams->nStratId
									<< "] NetStratExecs [" << nNetStratExecs
									<< "] OfferHedge WorkingSize [" <<  BidHedgeOrder.GetWorkingSize()
									<< "] - Offer Quote DriverLastPrice [" <<  dDriverLastPrice
									<< "] DriverLastQty [" << nDriverLastQty
									<< "] : Setting new HedgeBid Qty = [" << nNewHedgeQty
									<< "] at Price = [" << dNewHedgePrice
									<< "] "
									<< std::endl);
			}

			bool bStreetSent = SendOutStreetOrder(BidHedgeOrder, pStratParams->szDest, nNewHedgeQty, dNewHedgePrice);

			if (bStreetSent)
				return SUCCESS;
			else
				return FAILURE;
		}
	}
	return FAILURE;
}

int	BidOfferSpread::HandleQuoteUponMarketData (StratParams* pStratParams, MTICK& mtick_quote, MTICK& mtick_hedge, bool bBidSpread)
{
	// STEP 1 : Get client order
	CLIENT_ORDER QuoteOrder;
	GetQuoteClientOrder(QuoteOrder, pStratParams, bBidSpread);

	// STEP 2 : Calculate order quantity
	int nNewOrderQty = 0;
	nNewOrderQty = CalculateNewQuoteQuantity(pStratParams, bBidSpread);
	if (nNewOrderQty <= 0)
	{
		CERR << "StratId [" << pStratParams->nStratId
				<<"] Invalid new OrdQty [" << nNewOrderQty
				<<"] : Not sending new " << (bBidSpread?"Bid":"Offer")
				<<" quote order!"
				<< std::endl;
		return FAILURE;
	}

	if (pStratParams->eStratMode == STRAT_MODE_AGGRESSIVE)
	{
		int nQuoteQtyLimit= CalculateQuoteQtyLimit(mtick_quote, bBidSpread);

		_STRAT_LOG_DBUG_(CDEBUG	<< "StratId [" << pStratParams->nStratId
						<< "] Quote Mode [" << StratModeToStr(pStratParams->eStratMode)
						<< "] Symbol [" << mtick_quote.GetSymbol()
						<< "] NewOrderQty [" << nNewOrderQty
						<< "] QtyLimit = [" << nQuoteQtyLimit
						<< "] : Adjusting nNewOrderQty = [" << (nNewOrderQty<nQuoteQtyLimit?nNewOrderQty:nQuoteQtyLimit)
						<< "]"
						<< std::endl);

		nNewOrderQty = min (nNewOrderQty, nQuoteQtyLimit);

		_STRAT_LOG_VERB_(CWARN	<< "StratId [" << pStratParams->nStratId
							<< "] Quote Mode [" << StratModeToStr(pStratParams->eStratMode)
							<< "] Symbol [" << mtick_quote.GetSymbol()
							<< "] : Adjusted nNewOrderQty = [" << nNewOrderQty
							<< "]"
							<< std::endl);
	}

	// STEP 3 : Calculate order price
	double dQuotePrice = 0.0, dBenchmark=0.0;
	dBenchmark = bBidSpread? GetBiddingBenchmark(pStratParams) : GetOfferingBenchmark(pStratParams);
	dQuotePrice = GetStratQuotePrice(dBenchmark, mtick_quote, mtick_hedge, bBidSpread);

	if (QuoteOrder.GetWorkingSize() == 0)
	{
		// Send new bid quote with qty => nNewOrderQty
		bool bStreetSent = SendOutStreetOrder(QuoteOrder, pStratParams->szDest, nNewOrderQty, dQuotePrice);
		if (!bStreetSent)
		{
			CERR << "StratID [" << pStratParams -> nStratId
				<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
				<< "] Symbol [" << pStratParams->szSymbolQuote
				<< "] : FAILED to send new " << (bBidSpread?"Bidding":"Offering")
				<< " Quote at Price [" << dQuotePrice
				<< "] Quantity [" << nNewOrderQty
				<< "]"
				<< std::endl;

			return FAILURE;
		}
		else
		{
			CINFO << "StratID [" << pStratParams -> nStratId
					<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
					<< "] Symbol [" << pStratParams->szSymbolQuote
					<< "] : NEW " << (bBidSpread?"Bidding":"Offering")
					<< " Quote sent at Price [" << dQuotePrice
					<< "] Quantity [" << nNewOrderQty
					<< "]"
					<< std::endl;

			return SUCCESS;
		}
	}
	else
	{
		// Replace existing quote by change to -> nNewOrderQty
		STREET_ORDER_CONTAINER SOrdContainer;
		STREET_ORDER ExistingStreetOrder;

		if(SOrdContainer.GetFirstActiveByClientOrderId(ExistingStreetOrder, QuoteOrder.GetHandle()))
		{
			if (ReplaceRiskLimitControl(QuoteOrder) == SUCCESS)
			{
				do
				{
					HandleSHFEReplace(ExistingStreetOrder, nNewOrderQty, dQuotePrice);

					PublishStratStatus(pStratParams);
				}
				while((ReplaceRiskLimitControl(QuoteOrder) == SUCCESS)
						&& (SOrdContainer.GetNextActiveByClientOrderId(ExistingStreetOrder)));
			}
			else
			{
				CERR << "StratID [" << pStratParams->nStratId
					<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
					<<"] Replacing " <<  (bBidSpread?"Bid":"Offer")
					<<" Quote : Will breach MaxRplLimit [" << pStratParams->nMaxRplLimit
					<<"] - NOT sending further cancel request!"
					<<std::endl;

				PublishStratStatus(pStratParams);

				return FAILURE;
			}
		}

		PublishStratStatus(pStratParams);

		return SUCCESS;
	}
}

int	BidOfferSpread::HandleQuoteUponInvalidSpread(StratParams* pStratParams, bool bBidSpread)
{
	// STEP 1 : Get client order
	CLIENT_ORDER QuoteOrder;
	GetQuoteClientOrder(QuoteOrder, pStratParams, bBidSpread);

	if (QuoteOrder.GetWorkingSize() == 0)
	{
		_STRAT_LOG_DBUG_(CDEBUG << "Strat ID [" << pStratParams->nStratId
									<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
									<< "] " << (bBidSpread?"Bid":"Offer")
									<< "] Bench no longer satisfied for QuoteOrder ID [" << QuoteOrder.GetHandle()
									<< "] : No active Street Orders!"
									<< "]"
									<< std::endl);
		return SUCCESS;
	}
	else
	{
		_STRAT_LOG_VERB_(CWARN << "Strat ID [" << pStratParams->nStratId
								<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
								<< "] " << (bBidSpread?"Bid":"Offer")
								<< "] Bench no longer satisfied for QuoteOrder ID [" << QuoteOrder.GetHandle()
								<< "] : Cancelling Street Orders!"
								<< "]"
								<< std::endl);

		STREET_ORDER_CONTAINER SOrdContainer;
		STREET_ORDER ExistingStreetOrder;

		if(SOrdContainer.GetFirstActiveByClientOrderId(ExistingStreetOrder, QuoteOrder.GetHandle()))
		{
			if (ReplaceRiskLimitControl(QuoteOrder) == SUCCESS)
			{
				do
				{
					ExistingStreetOrder.Cancel();

					PublishStratStatus(pStratParams);
				}
				while((ReplaceRiskLimitControl(QuoteOrder) == SUCCESS)
						&& (SOrdContainer.GetNextActiveByClientOrderId(ExistingStreetOrder)));
			}
			else
			{
				CERR << "StratID [" << pStratParams->nStratId
					<< "] QuoteMode [" << StratModeToStr(pStratParams->eStratMode)
					<<"] Replacing " <<  (bBidSpread?"Bid":"Offer")
					<<" Quote : Will breach MaxRplLimit [" << pStratParams->nMaxRplLimit
					<<"] - NOT sending further cancel request!"
					<<std::endl;

				PublishStratStatus(pStratParams);

				return FAILURE;
			}
		}

		PublishStratStatus(pStratParams);

		return SUCCESS;

//		return CancelAllOrdersForClientOrder(QuoteOrder.GetHandle());
	}
}


int BidOfferSpread::HandleHedgeOnQuoteExecs(STREET_EXEC& StreetExec, CLIENT_ORDER& ParentOrder)
{
	StratParams *pStratParams = NULL;
	pStratParams = GetActiveStratParamsByClientOrderId(ParentOrder.GetHandle());
	if (!pStratParams)
	{
		_STRAT_LOG_VERB_(CERR << "Failed to get StratParam with ClientOrderId [" << ParentOrder.GetHandle() << "]" << std::endl);
		return FAILURE;
	}

	int nNetStratExecs = 0;
	nNetStratExecs = CalculateNetStratExecs(pStratParams);

	bool bHedgeAdjusted = IsHedgeAdjustComplete (pStratParams, nNetStratExecs);

	// Handle Hedge
	if (!bHedgeAdjusted)
	{
		_STRAT_LOG_DBUG_(CDEBUG << "StratID [" << pStratParams->nStratId
								<< "] Hedge Adjusted? [" << bHedgeAdjusted
								<< "] - Proceed to Adjust Hedge Working Size!"
								<< "]"
								<< std::endl);

		if (HandleHedgeAdjustWorkingSize (pStratParams, nNetStratExecs, StreetExec.GetLastFillPrice(), StreetExec.GetLastFillQty(), StreetExec.GetClientId2()) == SUCCESS)
		{
			_STRAT_LOG_VERB_(CWARN << "StratID [" << pStratParams->nStratId
									<< "] : Successful in attempt to adjust Hedge working size."
									<< std::endl);
			return SUCCESS;
		}
		else
		{
			_STRAT_LOG_VERB_(CERR << "StratID [" << pStratParams->nStratId
									<< "] : FAIL in attempt to adjust Hedge working size!"
									<< std::endl);
			return FAILURE;
		}
	}

	return SUCCESS;

}

int	BidOfferSpread::HandleManualOrderUponCommand(long nStratId, const char* szSymbol, double dPrice, int nOrdQty, const char* szSide, int nLegId, const char* szInstr)
{
	// STEP 1: Get StratParams from Global Map gmStratMap
	STRATMAPITER mStIter = gmStratMap.find(nStratId);
	StratParams *pStratParams = NULL;
	if(mStIter != gmStratMap.end())
	{
		pStratParams = (StratParams*) mStIter->second;
		_STRAT_LOG_DBUG_(CDEBUG << "Found StratParams with StratID [" << nStratId << "]"  << std::endl);
	}
	else
	{
		_STRAT_LOG_VERB_(CWARN << "StratID [" << nStratId << "] not found in gmStratMap!" << std::endl);
		return FAILURE;
	}

	if(!pStratParams)
	{
		_STRAT_LOG_VERB_(CWARN << "Invalid StratParam Pointer!" << std::endl);
		return FAILURE;
	}

	// STEP 2: Get Active Client Order with the LegId
	CLIENT_ORDER ClientOrder;
	switch (nLegId)
	{
		case LEGID_BO_BQ:
		case LEGID_BO_OQ:
			_STRAT_LOG_VERB_(CWARN << "StratId [" << nStratId
									<< "] Symbol [" <<  szSymbol
									<< "] LegId [" << nLegId
									<< "] : Not sending order on Quote Contract!"
									<< std::endl);
			return FAILURE;
			break;
		case LEGID_BO_BH:
			GetHedgeClientOrder(ClientOrder, pStratParams, true);
			break;
		case LEGID_BO_OH:
			GetHedgeClientOrder(ClientOrder, pStratParams, false);
			break;
		default :
			_STRAT_LOG_VERB_(CERR << "Failed to match LegId [" << nLegId
									<< "] from command!" << std::endl);
			return FAILURE;
	}

	// STEP 3: Sanity check on Symbol and Side
	enOrderSide eSide = SIDE_INVALID;
	if (!strcmp(szSide, "BUY"))			{	eSide=SIDE_BUY;		}
	else if (!strcmp(szSide, "SELL"))	{	eSide=SIDE_SELL;	}

	if (strcmp(ClientOrder.GetSymbol(), szSymbol) || ClientOrder.GetSide() != eSide )
	{
		_STRAT_LOG_VERB_(CERR << "StratId [" << nStratId
								<< "] LegId [" << nLegId
								<< "] ClientOrder ID [" << ClientOrder.GetHandle()
								<< "] Symbol [" <<  ClientOrder.GetSymbol()
								<< "] Side [" <<  ClientOrder.GetSide()
								<< "] : Does not match with command Symbol [" << szSymbol
								<< "] Side [" << eSide
								<< "]! - Not sending order with command!"
								<< std::endl);
		return FAILURE;
	}

	if(!strcmp(szInstr, "NEW_ORD"))
	{
		_STRAT_LOG_VERB_(CWARN << "StratID [" << pStratParams -> nStratId
								<< "] LegId [" << nLegId
								<< "] ClientOrder ID [" << ClientOrder.GetHandle()
								<< "] Symbol [" << szSymbol
								<< "] Instruction [" << szInstr
								<< "] : Sending NEW manual [" << szSide
								<< "] order sent at Price [" << dPrice
								<< "] Quantity [" << nOrdQty
								<< "]"
								<< std::endl);

		bool bStreetSent = SendOutStreetOrder(ClientOrder, pStratParams->szDest, nOrdQty, dPrice);

		if (!bStreetSent)
		{
			CERR << "StratID [" << pStratParams -> nStratId
					<< "] LegId [" << nLegId
					<< "] ClientOrder ID [" << ClientOrder.GetHandle()
					<< "] Symbol [" << szSymbol
					<< "] : FAILED to send NEW manual [" << szSide
					<< "] order at Price [" << dPrice
					<< "] Quantity [" << nOrdQty
					<< "]"
					<< std::endl;

			return FAILURE;
		}
		else
		{
			CINFO << "StratID [" << pStratParams -> nStratId
					<< "] LegId [" << nLegId
					<< "] ClientOrder ID [" << ClientOrder.GetHandle()
					<< "] Symbol [" << szSymbol
					<< "] : NEW manual [" << szSide
					<< "] order sent at Price [" << dPrice
					<< "] Quantity [" << nOrdQty
					<< "]"
					<< std::endl;

			return SUCCESS;
		}
	}

	return SUCCESS;
}


double BidOfferSpread::GetStratQuotePrice (double dBenchmark, MTICK& mtick_quote, MTICK& mtick_hedge, bool bBidSpread)
{
	double dQuotePrice = 0.0;
	double dTickSizeQuote = 0.0001;
	dTickSizeQuote = mtick_quote.GetTickSize();

	if(!(dTickSizeQuote > g_dEpsilon))
	{
		_STRAT_LOG_VERB_(CERR << "Invalid Tick Size in MTICK :  Symbol [" <<  mtick_quote.GetSymbol()
				<< "] TickSize [" << dTickSizeQuote
				<< "] "
				<< std::endl);
		return dQuotePrice;
	}

	int nBenchTicks = int (dBenchmark / dTickSizeQuote);

	if (bBidSpread)
	{
		// For Bidding, round-down to nearest tick
		dQuotePrice = mtick_hedge.GetBid() + nBenchTicks * dTickSizeQuote;
		if (dQuotePrice - mtick_quote.GetAsk() > g_dEpsilon)	{	dQuotePrice = mtick_quote.GetAsk();	}
	}
	else
	{
		// For Offering, round-up to nearest tick
		if (dBenchmark-nBenchTicks*dTickSizeQuote > g_dEpsilon)
		{
			nBenchTicks++;
		}
		dQuotePrice = mtick_hedge.GetAsk() + nBenchTicks * dTickSizeQuote;
		if (mtick_quote.GetBid() - dQuotePrice > g_dEpsilon)	{	dQuotePrice = mtick_quote.GetBid();	}
	}

	CINFO << "Quote Symbol [" << mtick_quote.GetSymbol()
			<< "] TickSize [" << dTickSizeQuote
			<< "] " << (bBidSpread?"Bid":"Offer") << " Benchmark [" << dBenchmark
			<< "] BenchTicks [" << nBenchTicks
			<< "] - Quote Bid/Ask [" << mtick_quote.GetBid() << "/" << mtick_quote.GetAsk()
			<< "] Hedge Bid/Ask [" <<  mtick_hedge.GetBid() << "/" << mtick_hedge.GetAsk()
			<< "] : Setting New Price = [" << dQuotePrice
			<< "]"
			<< std::endl;

	return dQuotePrice;
}

double BidOfferSpread::GetStratHedgePrice (StratParams* pStratParams, double dDriverLastPrice, bool bBidHedge)
{
	double dBenchmark = 0.0, dHedgePrice = 0.0, dTickSizeHedge = g_dEpsilon/10.0;

	dTickSizeHedge = GetTickSizeForSymbol(pStratParams->szSymbolHedge);

	if(!(dTickSizeHedge > g_dEpsilon))
	{
		_STRAT_LOG_VERB_(CERR << "Failed to get valid tick size for:  Symbol [" << pStratParams->szSymbolHedge
				<< "] TickSize [" << dTickSizeHedge
				<< "] "
				<< std::endl);
		return dHedgePrice;
	}

//	dBenchmark = bBidHedge? GetOfferingBenchmark(pStratParams) : GetBiddingBenchmark(pStratParams);
	dBenchmark = bBidHedge? pStratParams->dStateOfferBench :  pStratParams->dStateBidBench;

	int nBenchTicks = int (dBenchmark / dTickSizeHedge);

	if (bBidHedge)
	{
		// For Bidding, round-down to nearest tick
		if (dBenchmark-nBenchTicks*dTickSizeHedge > g_dEpsilon)
		{
			nBenchTicks++;
		}
		dHedgePrice = dDriverLastPrice - (nBenchTicks - pStratParams->nPayUpTicks) * dTickSizeHedge;
	}
	else
	{
		// For Offering, round-up to nearest tick
		dHedgePrice = dDriverLastPrice - (nBenchTicks + pStratParams->nPayUpTicks) * dTickSizeHedge;
	}

	CINFO << "Hedge Symbol [" << pStratParams->szSymbolHedge
			<< "] TickSize [" << dTickSizeHedge
			<< "] " << (bBidHedge?"Offer":"Bid") << " Bench [" << dBenchmark
			<< "] BenchTicks [" << nBenchTicks
			<< "] Quote Last Done Price [" << dDriverLastPrice
			<< "] PayUp Ticks [" << pStratParams->nPayUpTicks
			<< "] : Setting New " << (bBidHedge?"Bid":"Offer") <<" Hedge Price = [" << dHedgePrice
			<< "]"
			<< std::endl;

	return dHedgePrice;

}

int	BidOfferSpread::CalculateNewQuoteQuantity(StratParams* pStratParams, bool bBidSpread)
{
	int nOrdQty = 0, nStageCapacity=0;
	int nNetStratExecs = 0;
	nNetStratExecs = CalculateNetStratExecs(pStratParams);

	CLIENT_ORDER BidQuoteOrder, OfferQuoteOrder;

	GetQuoteClientOrder(BidQuoteOrder, pStratParams, true);
	GetQuoteClientOrder(OfferQuoteOrder, pStratParams, false);

	int nNetPos=0, nCurrStage=0, nStages=1, nMaxPos=0, nStageInterval=1;

	nStages = pStratParams->nNumStages;
	nMaxPos = pStratParams->nMaxPosition;
	nStageInterval =  (nMaxPos%nStages > 0)? int(nMaxPos/nStages)+1 : int(nMaxPos/nStages);
	nNetPos = BidQuoteOrder.GetStreetExecs() - OfferQuoteOrder.GetStreetExecs();

	if (bBidSpread)
	{
		if (nNetPos >= 0)
		{
			nCurrStage = int((abs(nNetPos))/nStageInterval);
			nStageCapacity = min (nMaxPos-nCurrStage*nStageInterval, (nCurrStage+1)*nStageInterval - abs(nNetPos));
		}
		else
		{
			nCurrStage = 0;
			nStageCapacity = min (nMaxPos-nCurrStage*nStageInterval, nStageInterval);
		}

	}
	else
	{
		if (nNetPos >= 0)
		{
			nCurrStage = 0;
			nStageCapacity = min (nMaxPos-nCurrStage*nStageInterval, nStageInterval);

		}
		else
		{
			nCurrStage = int((abs(nNetPos))/nStageInterval);
			nStageCapacity = min (nMaxPos-nCurrStage*nStageInterval, (nCurrStage+1)*nStageInterval - abs(nNetPos));
		}
	}

	_STRAT_LOG_VERB_(CWARN << "StratID [" << pStratParams -> nStratId
							<< "] QuoteSymbol [" << pStratParams->szSymbolQuote
							<< "] QtyPerOrder [" << pStratParams->nQtyPerOrder
							<< "] NumStages [" << pStratParams->nNumStages
							<< "] MaxPositionLimit [" << pStratParams->nMaxPosition
							<< "] StageInterval [" << nStageInterval
							<< "] - BotExecs [" << BidQuoteOrder.GetStreetExecs()
							<< "] SoldExecs [" <<  OfferQuoteOrder.GetStreetExecs()
							<< "] NetQuotePos [" << nNetPos
							<< "] : [" << (bBidSpread?"Bid":"Offer")
							<< "] CurrStage [" << nCurrStage
							<< "] -> Setting StageCapacity = [" << nStageCapacity
							<< "]"
							<< std::endl);

	if (nNetStratExecs == 0)
	{
		nOrdQty = min (nStageCapacity, pStratParams->nQtyPerOrder);
	}
	else if (nNetStratExecs > 0)
	{
		if (bBidSpread)
		{
			if (nNetStratExecs <= pStratParams->nQtyPerOrder)
			{
				nOrdQty = min (nStageCapacity, pStratParams->nQtyPerOrder - nNetStratExecs);
			}
			else
			{
				nOrdQty = 0;
			}
		}
		else
		{
			nOrdQty = min (nStageCapacity, pStratParams->nQtyPerOrder);
		}
	}
	else
	{
		if (!bBidSpread)
		{
			if (abs(nNetStratExecs) <= pStratParams->nQtyPerOrder)
			{
				nOrdQty = min (nStageCapacity, pStratParams->nQtyPerOrder - abs(nNetStratExecs));
			}
			else
			{
				nOrdQty = 0;
			}
		}
		else
		{
			nOrdQty = min (nStageCapacity, pStratParams->nQtyPerOrder);
		}
	}

	_STRAT_LOG_VERB_(CWARN << "StratID [" << pStratParams -> nStratId
							<< "] QuoteSymbol [" << pStratParams->szSymbolQuote
							<< "] QtyPerOrder [" << pStratParams->nQtyPerOrder
							<< "] QuoteSide [" << (bBidSpread?"Bid":"Offer")
							<< "] MaxPositionLimit [" << pStratParams->nMaxPosition
							<< "] NetStratExecs [" << nNetStratExecs
							<< "] StageCapacity [" << nStageCapacity
							<< "] MaxPositionLimit [" << pStratParams->nMaxPosition\
							<< "] -> Setting New Offer OrdQty = [" << nOrdQty
							<< "]"
							<< std::endl);

	return nOrdQty;

}

int	BidOfferSpread::CalculateQuoteQtyLimit(MTICK& mtick_quote, bool bBidSpread)
{
	int nMarketSize=0, nQuoteQtyLimit=0;

	nMarketSize = bBidSpread? mtick_quote.GetAskSize() : mtick_quote.GetBidSize();

	_STRAT_LOG_VERB_(CWARN << "Quote Symbol [" << mtick_quote.GetSymbol()
					<< "] " << (bBidSpread?"AskSize":"BidSize")
					<< " [" << (bBidSpread?mtick_quote.GetAskSize():mtick_quote.GetBidSize())
					<< "] : QuoteQtyLimit = [" << nQuoteQtyLimit
					<< "]"
					<< std::endl);

	nQuoteQtyLimit = int (nMarketSize / 2);

	return nQuoteQtyLimit;
}

int BidOfferSpread::HandleSHFEReplace(STREET_ORDER& StreetOrder, int nOrderQty, double dOrderPrice)
{
	STREET_ORDER ToSendStreetOrder;
	enOrderState eStreetOrdState = StreetOrder.GetOrderState();

	_STRAT_LOG_DBUG_(CDEBUG << "StreetOrder ID [" << StreetOrder.GetHandle()
					<< "] Symbol [" <<  StreetOrder.GetSymbol()
					<< "] OrdState [" << StateToStr(eStreetOrdState)
					<< "]"
					<< std::endl);

	if (eStreetOrdState == STATE_OPEN || eStreetOrdState == STATE_PARTIAL)
	{
		_STRAT_LOG_VERB_(CWARN << "STAGE 1 : StreetOrder ID [" << StreetOrder.GetHandle()
				<< "] Symbol [" <<  StreetOrder.GetSymbol()
				<< "] OrdState [" << StateToStr(eStreetOrdState)
				<< "] : Sending Cancel Request!"
				<< std::endl);

		if(!CanReplace(StreetOrder))
		{
			CERR << "STAGE 1 : Cannot Modify Street Order [" << StreetOrder.GetHandle()
					<<"] , OrderState [" << StateToStr(eStreetOrdState)
					<<"]"
					<< std::endl;
			return FAILURE;
		}

		if (dOrderPrice < g_dEpsilon)
		{
			CERR << "STAGE 1 : Invalid Price for Rpl Price [" << dOrderPrice << "]" << std::endl;
			return FAILURE;
		}

		if (nOrderQty <= 0)
		{
			CERR << "STAGE 1 : Invalid Quantity for Rpl Qty [" << nOrderQty << "]" << std::endl;
			return FAILURE;
		}

		if((fabs(dOrderPrice - StreetOrder.GetPrice()) < g_dEpsilon)
			&& StreetOrder.GetSize() == nOrderQty)
		{
			_STRAT_LOG_VERB_(CWARN << "STAGE 1 : Existing Street Order ID [" << StreetOrder.GetHandle()
					<< "] with Price [" << StreetOrder.GetPrice()
					<< "] Qty [" << StreetOrder.GetSize()
					<< "] same as Rpl Street Order ID [" << ToSendStreetOrder.GetHandle()
					<< "] Price [" << dOrderPrice
					<< "] Qty [" << nOrderQty
					<< "] : Keeping existing street order!"
					<< "]"
					<< std::endl);

			return FAILURE;
		}

		ToSendStreetOrder.SetOrderType(TYPE_LIMIT);
		ToSendStreetOrder.SetPrice(dOrderPrice);
		ToSendStreetOrder.SetSize(nOrderQty);

		ToSendStreetOrder.SetSymbol(StreetOrder.GetSymbol());
		ToSendStreetOrder.SetPortfolio(StreetOrder.GetPortfolio());
		ToSendStreetOrder.SetClientOrdId(StreetOrder.GetClientOrdId());
		ToSendStreetOrder.SetSide(StreetOrder.GetSide());
		ToSendStreetOrder.SetDestination(StreetOrder.GetDestination());
		ToSendStreetOrder.SetOrderTimeInForce(TIF_DAY);
		ToSendStreetOrder.SetWaveName(StreetOrder.GetWaveName());
		ToSendStreetOrder.SetClientId2(StreetOrder.GetClientId2());

		std::string strCurrStreetOID(StreetOrder.GetHandle());

		enErrorCode errStreetCancel = StreetOrder.Cancel();
		if(errStreetCancel != RULES_EO_OK)
		{
			CERR << "STAGE 1 : Cancel Fail for existing StreetOrder ID [" << StreetOrder.GetHandle()
					<<"] errStreetCancel [" << ErrorCodeToStr(errStreetCancel)
					<<"]"
					<< std::endl;
			return FAILURE;
		}
		else
		{
			gmSHFEOrdRplMap.insert(pair<std::string,STREET_ORDER>(strCurrStreetOID,ToSendStreetOrder));

			CINFO << "STAGE 1 : Successfully sent cancel request for [" << StreetOrder.GetHandle()
					<< "] Symbol [" << StreetOrder.GetSymbol()
					<< "] ClientOrder ID [" << StreetOrder.GetClientOrdId()
					<< "] :  Updated gmSHFEOrdRplMap!"
					<< std::endl;

			PrintSHFEOrdRplMap(gmSHFEOrdRplMap);

			return SUCCESS;
		}
	}
	else if (eStreetOrdState == STATE_CANCELLED)
	{
		_STRAT_LOG_VERB_(CWARN << "STAGE 2 : StreetOrder ID [" << StreetOrder.GetHandle()
				<< "] Symbol [" <<  StreetOrder.GetSymbol()
				<< "] OrdState [" << StateToStr(eStreetOrdState)
				<< "] : Sending New Order!"
				<< std::endl);

		SHFEORDERRPLMAPITER mShfeRplIter = gmSHFEOrdRplMap.find(string(StreetOrder.GetHandle()));
		if (mShfeRplIter == gmSHFEOrdRplMap.end())
		{
			CERR << "STAGE 2 : Cannot find StreetOrder ID [" << StreetOrder.GetHandle()
					<<"] in gmSHFEOrdRplMap! "
					<< std::endl;
			return FAILURE;
		}

		ToSendStreetOrder = (STREET_ORDER) mShfeRplIter->second;

		enErrorCode eReturn = ToSendStreetOrder.Send();
		if (eReturn != RULES_EO_OK)
		{
			CERR << "STAGE 2 : FAIL to send new StreetOrder ID [" << ToSendStreetOrder.GetHandle()
					<< "] to replace previous StreetOrder ID [" << StreetOrder.GetHandle()
					<< "] Symbol [" << StreetOrder.GetSymbol()
					<< "] ErrorCode [" << ErrorCodeToStr(eReturn)
					<< "] : Remove order [" << StreetOrder.GetHandle()
					<< "] from gmSHFEOrdRplMap!"
					<< std::endl;

			gmSHFEOrdRplMap.erase(mShfeRplIter);

			PrintSHFEOrdRplMap(gmSHFEOrdRplMap);

			// No further actions required while running algo
			// Otherwise should indicate replace failed and retrieve previous order

			return FAILURE;
		}
		else
		{
			CINFO << "STAGE 2 : Successfully sent new StreetOrder ID [" << ToSendStreetOrder.GetHandle()
					<< "] to replace previous StreetOrder ID [" << StreetOrder.GetHandle()
					<< "] Symbol [" << StreetOrder.GetSymbol()
					<< "] Portfolio [" << StreetOrder.GetPortfolio()
					<< "] Side [" << StreetOrder.GetSide()
					<< "] Price [" << StreetOrder.GetPrice()
					<< "] Quantity [" << StreetOrder.GetSize()
					<< "] Dest [" << StreetOrder.GetDestination()
					<< "] Wave [" << StreetOrder.GetWaveName()
					<< "] : Remove order [" << StreetOrder.GetHandle()
					<< "] from gmSHFEOrdRplMap!"
					<< std::endl;

			gmSHFEOrdRplMap.erase(mShfeRplIter);

			PrintSHFEOrdRplMap(gmSHFEOrdRplMap);

			// No further actions required while running algo
			// Otherwise should indicate replace success

			return SUCCESS;
		}
	}
	else if (eStreetOrdState == STATE_REJECTED)
	{
		_STRAT_LOG_VERB_(CWARN << "STAGE 3 : StreetOrder ID [" << StreetOrder.GetHandle()
						<< "] with RefOrdId [" << StreetOrder.GetRefOrdId()
						<< "] Symbol [" <<  StreetOrder.GetSymbol()
						<< "] OrdState [" << StateToStr(eStreetOrdState)
						<< "] : Cancel Request was REJECTED!"
						<< std::endl);

		SHFEORDERRPLMAPITER mShfeRplIter = gmSHFEOrdRplMap.find(string(StreetOrder.GetRefOrdId()));
		if (mShfeRplIter == gmSHFEOrdRplMap.end())
		{
			CERR << "STAGE 3 : Cannot find StreetOrder ID [" <<  StreetOrder.GetHandle()
					<< "] with RefOrdId ID [" << StreetOrder.GetRefOrdId()
					<<"] in gmSHFEOrdRplMap! "
					<< std::endl;
			return FAILURE;
		}

		ToSendStreetOrder = (STREET_ORDER) mShfeRplIter->second;

		CWARN << "STAGE 3 : CXL-REQ StreetOrder ID [" << StreetOrder.GetHandle()
				<<" on previous RefOrdIdID [" << StreetOrder.GetRefOrdId()
				<<"] was rejected! RejMsg [" << StreetOrder.GetRejectMsg()
				<<"] \n"
				<<"Thus discard new StreetOrder ID [" << ToSendStreetOrder.GetHandle()
				<< "] attempted to replace previous StreetOrder ID [" << StreetOrder.GetRefOrdId()
				<< "] Symbol [" << StreetOrder.GetSymbol()
				<< "] : Remove order [" << StreetOrder.GetRefOrdId()
				<< "] from gmSHFEOrdRplMap!"
				<< std::endl;

		gmSHFEOrdRplMap.erase(mShfeRplIter);

		PrintSHFEOrdRplMap(gmSHFEOrdRplMap);

		return SUCCESS;
	}
	else
	{
        _STRAT_LOG_DBUG_ (CDEBUG << "Callback in WRONG state. StreetOrderID [" << StreetOrder.GetHandle()
        		<< "] -> State [" << StateToStr(eStreetOrdState)
        		<< "]"
        		<< std::endl);
        return FAILURE;
	}
}

int	BidOfferSpread::OrderPriceCrossCheck(STREET_ORDER& NewStreetOrder)
{
	STREET_ORDER_CONTAINER SOC;
	STREET_ORDER so;

	if(SOC.GetFirstActiveBySymbol(so,NewStreetOrder.GetSymbol()))
	{
		do
		{
			// New Buy Order Price must be less than existing Sell Order Price
			// New Sell Order Price must be greater than existing Buy Order Price

			if (!strcmp(NewStreetOrder.GetDestination(), so.GetDestination()))
			{
				_STRAT_LOG_VERB_(CWARN << "NewStreetOrder ID [" << NewStreetOrder.GetHandle()
										<< "] to Destination [" << NewStreetOrder.GetDestination()
										<< "] Side [" << (NewStreetOrder.GetSide()==SIDE_BUY?"BUY":"SEL")
										<< "] at Price [" << NewStreetOrder.GetPrice()
										<< "] Comparing against previous StreetOrder ID [" << so.GetHandle()
										<< "] at Destination [" << so.GetDestination()
										<< "] Side [" <<  (so.GetSide()==SIDE_BUY?"BUY":"SEL")
										<< "] at Price [" << so.GetPrice()
										<< "]."
										<<std::endl);

				if ((NewStreetOrder.GetSide()==SIDE_BUY && so.GetSide()==SIDE_SELL
						&& (NewStreetOrder.GetPrice() - so.GetPrice() > -g_dEpsilon))
					|| (NewStreetOrder.GetSide()==SIDE_SELL && so.GetSide()==SIDE_BUY
						&& (NewStreetOrder.GetPrice() - so.GetPrice() < g_dEpsilon)))
				{
					NewStreetOrder.SetRejectMsg("Price SELF-CROSS!");

					_STRAT_LOG_VERB_(CERR << "NewStreetOrder ID [" << NewStreetOrder.GetHandle()
								<< "] to Destination [" << NewStreetOrder.GetDestination()
								<< "] Side [" << (NewStreetOrder.GetSide()==SIDE_BUY?"BUY":"SEL")
								<< "] at Price [" << NewStreetOrder.GetPrice()
								<< "] CORSSED with previous StreetOrder ID [" << so.GetHandle()
								<< "] at Destination [" << so.GetDestination()
								<< "] Side [" <<  (so.GetSide()==SIDE_BUY?"BUY":"SEL")
								<< "] at Price [" << so.GetPrice()
								<< "] : NOT sending new order!!!"
								<<std::endl);
					return FAILURE;
				}
			}
		}while(SOC.GetNextActiveBySymbol(so));
	}
	return SUCCESS;
}

int BidOfferSpread::ExecsRiskLimitControl(STREET_EXEC& StreetExec, CLIENT_ORDER& ParentOrder)
{
	// STEP 1 : Get active strat param by client order id
	StratParams *pStratParams = NULL;
	pStratParams = GetActiveStratParamsByClientOrderId(ParentOrder.GetHandle());
	if (!pStratParams)
	{
		_STRAT_LOG_VERB_(CERR << "Failed to get StratParam with ClientOrderId [" << ParentOrder.GetHandle() << "]" << std::endl);
		return FAILURE;
	}

	// STEP 2 : Get all client order and calculate current loss
	CLIENT_ORDER BidQuoteOrder, BidHedgeOrder, OfferQuoteOrder, OfferHedgeOrder;
	GetQuoteClientOrder(BidQuoteOrder, pStratParams, true);
	GetQuoteClientOrder(OfferQuoteOrder, pStratParams, false);
	GetHedgeClientOrder(BidHedgeOrder, pStratParams, true);
	GetHedgeClientOrder(OfferHedgeOrder, pStratParams, false);

	// Limit check I: MAX_LOSS
	// STEP I.1: Get current MID price for the two symbols
	MTICK mtick_quote, mtick_hedge;
	double dMidQuote=0.0, dMidHedge=0.0;
	MarketDataGetSymbolInfo(pStratParams->szSymbolQuote, mtick_quote);
	MarketDataGetSymbolInfo(pStratParams->szSymbolHedge, mtick_hedge);
	dMidQuote = (mtick_quote.GetAsk() + mtick_quote.GetBid())/2.0;
	dMidHedge = (mtick_hedge.GetAsk() + mtick_hedge.GetBid())/2.0;

	// SETP I.2: Calculate current loss
	double dNewAvgPrice=0.0, dUnitPrice=0.0, dNetLossQuote=0.0, dNetLossHedge=0.0;

	dUnitPrice = GetContractUnitPriceForSymbol(BidQuoteOrder.GetSymbol());
	if (dUnitPrice < g_dEpsilon/10.0)
	{
		CERR << "Failed to get Unit Price for Symbol [" << BidQuoteOrder.GetSymbol()
				<< "] - Cannot perform Max Loss check!"
				<< std::endl;
		return FAILURE;
	}

	if (StreetExec.GetClientId2() == LEGID_BO_BQ)
	{
		dNewAvgPrice = ((BidQuoteOrder.GetStreetExecs()-StreetExec.GetLastFillQty())*BidQuoteOrder.GetAvgPrice()+StreetExec.GetLastFillQty()*StreetExec.GetLastFillPrice())/(BidQuoteOrder.GetStreetExecs())*1.0;
		BidQuoteOrder.SetAvgPrice(dNewAvgPrice);
	}
	else if (StreetExec.GetClientId2() == LEGID_BO_OQ)
	{
		dNewAvgPrice = ((OfferQuoteOrder.GetStreetExecs()-StreetExec.GetLastFillQty())*OfferQuoteOrder.GetAvgPrice()+StreetExec.GetLastFillQty()*StreetExec.GetLastFillPrice())/(OfferQuoteOrder.GetStreetExecs())*1.0;
		OfferQuoteOrder.SetAvgPrice(dNewAvgPrice);
	}
	else if (StreetExec.GetClientId2() == LEGID_BO_BH)
	{
		dNewAvgPrice = ((BidHedgeOrder.GetStreetExecs()-StreetExec.GetLastFillQty())*BidHedgeOrder.GetAvgPrice()+StreetExec.GetLastFillQty()*StreetExec.GetLastFillPrice())/(BidHedgeOrder.GetStreetExecs())*1.0;
		BidHedgeOrder.SetAvgPrice(dNewAvgPrice);
	}
	else if (StreetExec.GetClientId2() == LEGID_BO_OH)
	{
		dNewAvgPrice = ((OfferHedgeOrder.GetStreetExecs()-StreetExec.GetLastFillQty())*OfferHedgeOrder.GetAvgPrice()+StreetExec.GetLastFillQty()*StreetExec.GetLastFillPrice())/(OfferHedgeOrder.GetStreetExecs())*1.0;
		OfferHedgeOrder.SetAvgPrice(dNewAvgPrice);
	}
	else
	{
		CERR << "Failed to map executions with GetClientId2 [" << StreetExec.GetClientId2()
				<< "] to StratID [" << pStratParams->nStratId
				<< "] ! : ParentOrd ID [" << ParentOrder.GetHandle()
				<< "] - BidQuoteOrder ID [" << BidQuoteOrder.GetHandle()
				<< "] OfferQuoteOrder ID [" << OfferQuoteOrder.GetHandle()
				<< "] BidHedgeOrder ID [" << BidHedgeOrder.GetHandle()
				<< "] OfferHedgeOrder ID [" << OfferHedgeOrder.GetHandle()
				<< "]"
				<< std::endl;
	}

	dNetLossQuote = dUnitPrice * (BidQuoteOrder.GetStreetExecs() * (BidQuoteOrder.GetAvgPrice()-dMidQuote)
					+ OfferQuoteOrder.GetStreetExecs() * (dMidQuote-OfferQuoteOrder.GetAvgPrice()));

	dNetLossHedge = dUnitPrice * (BidHedgeOrder.GetStreetExecs() * (BidHedgeOrder.GetAvgPrice()-dMidHedge)
						+ OfferHedgeOrder.GetStreetExecs() * (dMidHedge-OfferHedgeOrder.GetAvgPrice()));

	CWARN << "StratID [" << pStratParams->nStratId
			<< "] QuoteSymbol [" << pStratParams->szSymbolQuote
			<< "] BidQuoteExec [" << BidQuoteOrder.GetStreetExecs()
			<< "] @ AvgPrice [" << BidQuoteOrder.GetAvgPrice()
			<< "]  OfferQuoteExec [" << OfferQuoteOrder.GetStreetExecs()
			<< "] @ AvgPrice [" << OfferQuoteOrder.GetAvgPrice()
			<< "] vs QuoteMid (Ask+Bid)/2 [" << dMidQuote <<"] (" << mtick_quote.GetAsk() << "+" << mtick_quote.GetBid()
			<< ")/2 with UnitPrice [" << dUnitPrice
			<< "] - NetQuoteLoss = [" << dNetLossQuote << "]"
			<< std::endl;
	CWARN << "StratID [" << pStratParams->nStratId
			<< "] HedgeSymbol [" << pStratParams->szSymbolHedge
			<< "]  BidHedgeExec [" << BidHedgeOrder.GetStreetExecs()
			<< "] @ AvgPrice [" << BidHedgeOrder.GetAvgPrice()
			<< "]  OfferHedgeExec ["  << OfferHedgeOrder.GetStreetExecs()
			<< "] @ AvgPrice [" << OfferHedgeOrder.GetAvgPrice()
			<< "] vs QuoteMid (Ask+Bid) [" << dMidHedge <<"] (" << mtick_hedge.GetAsk() << "+" << mtick_hedge.GetBid()
			<< ")/2 with UnitPrice [" << dUnitPrice
			<< "] - NetHedgeLoss = [" << dNetLossHedge << "]"
			<< std::endl;

	// SETP I.3: check MAX_LOSS limit
	if (dNetLossQuote + dNetLossHedge - pStratParams->dMaxLoss >= g_dEpsilon)
	{
		CERR << "StratID [" << pStratParams->nStratId
				<< "] Total Loss [" << dNetLossQuote+dNetLossHedge
				<< "] EXCEEDS MaxLossLimit [" << pStratParams->dMaxLoss
				<< "] : STOP Strategy and CANCEL all street orders!!!"
				<< std::endl;

		StopStratOnRiskLimitBreached(pStratParams);

		PublishStratLoss(pStratParams, mtick_quote, mtick_hedge, dUnitPrice, dNetLossQuote, dNetLossHedge, BidQuoteOrder, BidHedgeOrder, OfferQuoteOrder, OfferHedgeOrder);

		return FAILURE;
	}
	else
	{
		_STRAT_LOG_DBUG_(CWARN << "StratID [" << pStratParams->nStratId
								<< "] Total Loss [" << dNetLossQuote+dNetLossHedge
								<< "] within MaxLossLimit [" << pStratParams->dMaxLoss
								<< "]."
								<<std::endl);

		PublishStratLoss(pStratParams, mtick_quote, mtick_hedge, dUnitPrice, dNetLossQuote, dNetLossHedge, BidQuoteOrder, BidHedgeOrder, OfferQuoteOrder, OfferHedgeOrder);

	}

	// Limit check II: MAX_POS
	// STEP II.1 : Get current positions
	int nNetPosition=0;
	nNetPosition = BidQuoteOrder.GetStreetExecs() - OfferQuoteOrder.GetStreetExecs();

	// STEP II.2 : Check MAX_POS limit
	if (abs(nNetPosition) >= pStratParams->nMaxPosition)
	{
		CWARN << "StratID [" << pStratParams->nStratId
				<< "] QuoteSymbol [" << pStratParams->szSymbolQuote
				<< "] BidQuoteExecs [" << BidQuoteOrder.GetStreetExecs()
				<< "] OfferQuoteExecs [" << OfferQuoteOrder.GetStreetExecs()
				<< "] NetPosition [" << nNetPosition
				<< "] EXCEEDS MaxPosLimit [+/- " << pStratParams->nMaxPosition
				<< "] : Should stopping quoting on [" << (((bool)(nNetPosition>0))?"Bid":"Offer")
				<< "] side!"
				<<std::endl;
	}
	else
	{
		_STRAT_LOG_VERB_(CWARN << "StratID [" << pStratParams->nStratId
								<< "] QuoteSymbol [" << pStratParams->szSymbolQuote
								<< "] BidQuoteExecs [" << BidQuoteOrder.GetStreetExecs()
								<< "] OfferQuoteExecs [" << OfferQuoteOrder.GetStreetExecs()
								<< "] NetPosition [" << nNetPosition
								<< "] within MaxPosLimit [+/- " << pStratParams->nMaxPosition
								<<"]."
								<<std::endl);
	}

	// All execs limit checks passed.
	return SUCCESS;
}

int BidOfferSpread::UpdateStratQuoteCxlRplCount(STREET_ORDER& StreetOrder, CLIENT_ORDER& ParentOrder)
{
	if (StreetOrder.GetClientId2() == LEGID_BO_BQ || StreetOrder.GetClientId2() == LEGID_BO_OQ)
	{
		StratParams *pStratParams = NULL;
		pStratParams = GetActiveStratParamsByClientOrderId(ParentOrder.GetHandle());
		if (!pStratParams)
		{
			_STRAT_LOG_VERB_(CERR << "Failed to get StratParam with ClientOrderId [" << ParentOrder.GetHandle() << "]" << std::endl);
			return FAILURE;
		}

		_STRAT_LOG_VERB_(CWARN	<< "StratID [" << pStratParams->nStratId
								<< "] ClientOrder ID [" << ParentOrder.GetHandle()
								<< "] Symbol [" << ParentOrder.GetSymbol()
								<< "] StreetOrder ID [" << StreetOrder.GetSymbol()
								<< "] : Increasing QuoteRplCount from [" << pStratParams->nQuoteRplCount
								<< "] to [ " << (pStratParams->nQuoteRplCount+1)
								<< "]"
								<< std::endl);

		pStratParams->nQuoteRplCount++;

		PublishStratStatus(pStratParams);

		return SUCCESS;
	}
	else
	{
		return SUCCESS;
	}
}

int BidOfferSpread::ReplaceRiskLimitControl(CLIENT_ORDER& ClientOrder)
{
	// STEP 1 : Get active strat param by client order id
	StratParams *pStratParams = NULL;
	pStratParams = GetActiveStratParamsByClientOrderId(ClientOrder.GetHandle());
	if (!pStratParams)
	{
		_STRAT_LOG_VERB_(CERR << "Failed to get StratParam with ClientOrderId [" << ClientOrder.GetHandle() << "]" << std::endl);
		return FAILURE;
	}

	_STRAT_LOG_VERB_( CWARN << "StratID [" << pStratParams->nStratId
							<< "] Quote Rpl Count [" << pStratParams->nQuoteRplCount
							<< "]. "
							<< std::endl);

	if (pStratParams->nMaxRplLimit >0 && (pStratParams->nQuoteRplCount >= pStratParams->nMaxRplLimit))
	{
		CERR << "StratID [" << pStratParams->nStratId
				<< "] QuoteRplCount [" << pStratParams->nQuoteRplCount
				<< "] EXCEEDS nMaxRplLimit [ " << pStratParams->nMaxRplLimit
				<< "] : STOP Strategy and CANCEL all street orders!!!"
				<<std::endl;

		StopStratOnRiskLimitBreached(pStratParams);

		return FAILURE;

	}
	else
	{
		_STRAT_LOG_VERB_( CDEBUG << "StratID [" << pStratParams->nStratId
									<< "] QuoteRplCount [" << pStratParams->nQuoteRplCount
									<< "] within nMaxRplLimit [ " << pStratParams->nMaxRplLimit
									<<"]."
									<<std::endl;);
	}

	return SUCCESS;

}

int BidOfferSpread::UpdateStratParamWithClientRpld(StratParams* pStratParams, StratParams* pStratParamsRpl)
{
	if (pStratParams == NULL || pStratParamsRpl == NULL)
	{
		CERR << "Invalid Param pointer. Cannot update State!" << std::endl;
		return FAILURE;
	}

	// Update Quote RPLD count
	if (pStratParams->nQuoteRplCount > 0)
	{
		_STRAT_LOG_VERB_(CWARN << "Getting pStratParams nQuoteRplCount ["  << pStratParams->nQuoteRplCount
								<< "] and Setting StratParamsRpl nQuoteRplCount from [" << pStratParamsRpl->nQuoteRplCount
								<< "] to [" << pStratParams->nQuoteRplCount
								<< "]."
								<< std::endl);
		pStratParamsRpl->nQuoteRplCount = pStratParams->nQuoteRplCount;
	}
	else
	{
		_STRAT_LOG_VERB_(CWARN << "Getting pStratParams nQuoteRplCount ["  << pStratParams->nQuoteRplCount
								<< "] and Keeping StratParamsRpl nQuoteRplCount as [" << pStratParamsRpl->nQuoteRplCount
								<< "]."
								<< std::endl);
	}

	// Update Hedge RPLD count
	if (pStratParams->nHedgeRplCount > 0)
	{
		_STRAT_LOG_VERB_(CWARN << "Getting pStratParams nHedgeRplCount ["  << pStratParams->nHedgeRplCount
								<< "] and Setting StratParamsRpl nHedgeRplCount from [" << pStratParamsRpl->nHedgeRplCount
								<< "] to [" << pStratParams->nHedgeRplCount
								<< "]."
								<< std::endl);
		pStratParamsRpl->nHedgeRplCount = pStratParams->nHedgeRplCount;
	}
	else
	{
		_STRAT_LOG_VERB_(CWARN << "Getting pStratParams nHedgeRplCount ["  << pStratParams->nHedgeRplCount
								<< "] and Keeping StratParamsRpl nHedgeRplCount as [" << pStratParamsRpl->nHedgeRplCount
								<< "]."
								<< std::endl);
	}

	PublishStratStatus(pStratParams);

	// Update Client Execs
	CLIENT_ORDER BidQuoteOrderOld, BidHedgeOrderOld, OfferQuoteOrderOld, OfferHedgeOrderOld;
	GetQuoteClientOrder(BidQuoteOrderOld, pStratParams, true);
	GetQuoteClientOrder(OfferQuoteOrderOld, pStratParams, false);
	GetHedgeClientOrder(BidHedgeOrderOld, pStratParams, true);
	GetHedgeClientOrder(OfferHedgeOrderOld, pStratParams, false);

	CLIENT_ORDER BidQuoteOrderRpl, BidHedgeOrderRpl, OfferQuoteOrderRpl, OfferHedgeOrderRpl;
	GetQuoteClientOrder(BidQuoteOrderRpl, pStratParamsRpl, true);
	GetQuoteClientOrder(OfferQuoteOrderRpl, pStratParamsRpl, false);
	GetHedgeClientOrder(BidHedgeOrderRpl, pStratParamsRpl, true);
	GetHedgeClientOrder(OfferHedgeOrderRpl, pStratParamsRpl, false);

	_STRAT_LOG_DBUG_(CDEBUG << "COMPARE: pStratParams BidQuoteOrderOld GetStreetExecs ["  << BidQuoteOrderOld.GetStreetExecs()
								<< "] @ AvgPrice [" << BidQuoteOrderOld.GetAvgPrice()
								<< "] - pStratParamsRpl BidQuoteOrderRpl GetStreetExecs[" << BidQuoteOrderRpl.GetStreetExecs()
								<< "] @ AvgPrce [" << BidQuoteOrderRpl.GetAvgPrice()
								<< "]."
								<< std::endl);

	_STRAT_LOG_DBUG_(CDEBUG << "COMPARE: pStratParams OfferQuoteOrderOld GetStreetExecs ["  << OfferQuoteOrderOld.GetStreetExecs()
								<< "] @ AvgPrice [" << OfferQuoteOrderOld.GetAvgPrice()
								<< "] - pStratParamsRpl OfferQuoteOrderRpl GetStreetExecs[" << OfferQuoteOrderRpl.GetStreetExecs()
								<< "] @ AvgPrce [" << OfferQuoteOrderRpl.GetAvgPrice()
								<< "]."
								<< std::endl);

	_STRAT_LOG_DBUG_(CDEBUG << "COMPARE: pStratParams BidHedgeOrderOld GetStreetExecs ["  << BidHedgeOrderOld.GetStreetExecs()
								<< "] @ AvgPrice [" << BidHedgeOrderOld.GetAvgPrice()
								<< "] - pStratParamsRpl BidHedgeOrderRpl GetStreetExecs[" << BidHedgeOrderRpl.GetStreetExecs()
								<< "] @ AvgPrce [" << BidHedgeOrderRpl.GetAvgPrice()
								<< "]."
								<< std::endl);

	_STRAT_LOG_DBUG_(CDEBUG << "COMPARE: pStratParams OfferHedgeOrderOld GetStreetExecs ["  << OfferHedgeOrderOld.GetStreetExecs()
								<< "] @ AvgPrice [" << OfferHedgeOrderOld.GetAvgPrice()
								<< "] - pStratParamsRpl OfferHedgeOrderRpl GetStreetExecs[" << OfferHedgeOrderRpl.GetStreetExecs()
								<< "] @ AvgPrce [" << OfferHedgeOrderRpl.GetAvgPrice()
								<< "]."
								<< std::endl);


	_STRAT_LOG_DBUG_(CDEBUG << "StratID: pStratParams StratID [" << pStratParams->nStratId
							<< "] Updated params of pStratParamsRpl StratID [" << pStratParamsRpl->nStratId
							<<"]"
							<< std::endl);


	_STRAT_LOG_VERB_(PrintStratOrderMap(gmStratMap, ((long)pStratParams->nStratId)));
	_STRAT_LOG_VERB_(PrintStratOrderMapRpl(gmStratMapRpl));

	return SUCCESS;
}

int	BidOfferSpread::PublishStratStatus(StratParams* pStratParams)
{
	if (!pStratParams)
	{
		CERR  << "Invalid pStratParams pointer!" << std::endl;
		return FAILURE;
	}

	if (g_SymConnection->GetFd() > 0)
	{
		  CCommand command("PUBLISH");
		  command.SetStringParam("TOPIC", pStratParams->szStratPort);
		  command.SetIntParam("STRATID", (int)pStratParams->nStratId);
		  command.SetStringParam("QUOTESYM", pStratParams->szSymbolQuote);
		  command.SetStringParam("HEDGESYM", pStratParams->szSymbolHedge);
		  command.SetIntParam("ACTIVE", (int)pStratParams->bStratRunning);
		  command.SetIntParam("QUOTERPL", pStratParams->nQuoteRplCount);
		  command.SetIntParam("HEDGERPL", pStratParams->nHedgeRplCount);
		  command.SetStringParam("TIME", Time());

		  CINFO << "Strat ID [" << pStratParams->nStratId
				  <<"] - Publishing Strat Status : TOPIC=[" << pStratParams->szStratPort
				  <<"] | STRATID=[" << (int)pStratParams->nStratId
				  <<"] | QUOTESYM=[" << pStratParams->szSymbolQuote
				  <<"] | HEDGESYM=[" << pStratParams->szSymbolHedge
				  <<"] | ACTIVE=[" << (int)pStratParams->bStratRunning
				  <<"] | QUOTERPL=[" << pStratParams->nQuoteRplCount
				  <<"] | HEDGERPL=[" << pStratParams->nHedgeRplCount
				  <<"] | TIME=[" << Time()
				  <<"]"
				  <<std::endl;

		  if(g_SymConnection->SendCommand(&command)==CCommand::INVALID_CMD_ID)
		  {
			  CWARN << "Unable to send command!" << std::endl;
		  }
		  else
		  {
			  CINFO << "Successfully sent command!" << std::endl;
		  }
	}

	return SUCCESS;
}

int	BidOfferSpread::PublishStratLoss(StratParams* pStratParams,  MTICK& mtick_quote, MTICK& mtick_hedge, double dUnitPrice, double dNetLossQuote, double dNetLossHedge, CLIENT_ORDER& BidQuoteOrder, CLIENT_ORDER& BidHedgeOrder, CLIENT_ORDER& OfferQuoteOrder, CLIENT_ORDER& OfferHedgeOrder)
{
	if (!pStratParams)
	{
		CERR  << "Invalid pStratParams pointer!" << std::endl;
		return FAILURE;
	}

	if (g_SymConnection->GetFd() > 0)
	{
		  CCommand command("PUBLISH");
		  command.SetStringParam("TOPIC", pStratParams->szStratPort);
		  command.SetIntParam("STRATID", (int)pStratParams->nStratId);
		  command.SetStringParam("QUOTESYM", pStratParams->szSymbolQuote);
		  command.SetStringParam("HEDGESYM", pStratParams->szSymbolHedge);
		  command.SetIntParam("ACTIVE", (int)pStratParams->bStratRunning);
		  command.SetIntParam("QUOTERPL", pStratParams->nQuoteRplCount);
		  command.SetIntParam("HEDGERPL", pStratParams->nHedgeRplCount);

		  command.SetDoubleParam("UNITPRICE", dUnitPrice);

		  command.SetIntParam("QUOTEBOTQTY", BidQuoteOrder.GetStreetExecs());
		  command.SetDoubleParam("QUOTEBOTAVG", BidQuoteOrder.GetAvgPrice());
		  command.SetIntParam("QUOTESOLDQTY", OfferQuoteOrder.GetStreetExecs());
		  command.SetDoubleParam("QUOTESOLDAVG", OfferQuoteOrder.GetAvgPrice());
		  command.SetDoubleParam("QUOTEBID", mtick_quote.GetBid());
		  command.SetDoubleParam("QUOTEASK", mtick_quote.GetAsk());
		  command.SetDoubleParam("QUOTEMID", (mtick_quote.GetBid()+mtick_quote.GetAsk())/2.0);
		  command.SetDoubleParam("QUOTELOSS", dNetLossQuote);

		  command.SetIntParam("HEDGEBOTQTY", BidHedgeOrder.GetStreetExecs());
		  command.SetDoubleParam("HEDGEBOTAVG", BidHedgeOrder.GetAvgPrice());
		  command.SetIntParam("HEDGESOLDQTY", OfferHedgeOrder.GetStreetExecs());
		  command.SetDoubleParam("HEDGESOLDAVG", OfferHedgeOrder.GetAvgPrice());
		  command.SetDoubleParam("HEDGEBID", mtick_hedge.GetBid());
		  command.SetDoubleParam("HEDGEASK", mtick_hedge.GetAsk());
		  command.SetDoubleParam("HEDGEMID", (mtick_hedge.GetBid()+mtick_hedge.GetAsk())/2.0);
		  command.SetDoubleParam("HEDGELOSS", dNetLossHedge);

		  command.SetStringParam("TIME", Time());

		  CINFO << "\nStrat ID [" << pStratParams->nStratId
				  <<"] - Publishing Strat Loss : TOPIC=[" << pStratParams->szStratPort
				  <<"] | STRATID=[" << (int)pStratParams->nStratId
				  <<"] | QUOTESYM=[" << pStratParams->szSymbolQuote
				  <<"] | HEDGESYM=[" << pStratParams->szSymbolHedge
				  <<"] | ACTIVE=[" << (int)pStratParams->bStratRunning
				  <<"] | QUOTERPL=[" << pStratParams->nQuoteRplCount
				  <<"] | HEDGERPL=[" << pStratParams->nHedgeRplCount
				  <<"]\n QUOTE: QUOTEBOTQTY=[" << BidQuoteOrder.GetStreetExecs()
				  <<"] | QUOTEBOTAVG=[" << BidQuoteOrder.GetAvgPrice()
				  <<"] | QUOTESOLDQTY=[" << OfferQuoteOrder.GetStreetExecs()
				  <<"] | QUOTESOLDAVG=[" << OfferQuoteOrder.GetAvgPrice()
				  <<"] | QUOTEBID=[" << mtick_quote.GetBid()
				  <<"] | QUOTEASK=[" << mtick_quote.GetAsk()
				  <<"] | QUOTEMID=[" << (mtick_quote.GetBid()+mtick_quote.GetAsk())/2.0
				  <<"] | QUOTELOSS=[" << dNetLossQuote
				  <<"]\n HEDGE: HEDGEBOTQTY=[" << BidHedgeOrder.GetStreetExecs()
				  <<"] | HEDGEBOTAVG=[" << BidHedgeOrder.GetAvgPrice()
				  <<"] | HEDGESOLDQTY=[" << OfferHedgeOrder.GetStreetExecs()
				  <<"] | HEDGESOLDAVG=[" << OfferHedgeOrder.GetAvgPrice()
				  <<"] | HEDGEBID=[" << mtick_hedge.GetBid()
				  <<"] | HEDGEASK=[" << mtick_hedge.GetAsk()
				  <<"] | HEDGEMID=[" << (mtick_hedge.GetBid()+mtick_hedge.GetAsk())/2.0
				  <<"] | HEDGELOSS=[" << dNetLossHedge
				  <<"]\n | TIME=[" << Time()
				  <<"]"
				  <<std::endl;

		  if(g_SymConnection->SendCommand(&command)==CCommand::INVALID_CMD_ID)
		  {
			  CWARN << "Unable to send command!" << std::endl;
		  }
		  else
		  {
			  CINFO << "Successfully sent command!" << std::endl;
		  }
	}

	return SUCCESS;
}


/// Utils
/// UTILITY FUNCTION: Send a new street order from client order
int BidOfferSpread::SendOutStreetOrder(CLIENT_ORDER& ClientOrder, const char* pszDestination, int nOrderQty, double dOrderPrice)
{
    if(!pszDestination)
    {
    	CERR << "Cannot Send StreetOrder. Destination is not set" << std::endl;
    	return FAILURE;
    }
    else if(nOrderQty <= 0)
    {
    	CERR << "Cannot Send StreetOrder. Invalid OrderQty [" << nOrderQty << "]" << std::endl;
    	return FAILURE;
    }

    STREET_ORDER order;
    if(dOrderPrice > g_dEpsilon)
    {
    	order.SetOrderType(TYPE_LIMIT);
    	order.SetPrice(dOrderPrice);
    }
    else
    {
    	CERR << "Cannot Send StreetOrder. Invalid Price [" << dOrderPrice << "]" << std::endl;
    	return FAILURE;
    }

    order.SetSymbol(ClientOrder.GetSymbol());
    order.SetPortfolio(ClientOrder.GetPortfolio());
    order.SetClientOrdId(ClientOrder.GetHandle());
    order.SetSide(ClientOrder.GetSide());

    order.SetDestination(pszDestination);
    order.SetSize(nOrderQty);
    order.SetOrderTimeInForce(TIF_DAY);

    order.SetWaveName(ClientOrder.GetFixTag(FIX_TAG_STRATPORT_BO));
    order.SetClientId2(atoi(ClientOrder.GetFixTag(FIX_TAG_LEGID_BO)));

    enErrorCode eReturn = order.Send();
    if (eReturn != RULES_EO_OK)
	{
		CERR << "FAIL to send street order : Client OrderID [" << ClientOrder.GetHandle()
				<< "] Street Order ID [" << order.GetHandle()
				<< "] Symbol [" << ClientOrder.GetSymbol()
				<< "] Portfolio [" << ClientOrder.GetPortfolio()
				<< "] Side [" << ClientOrder.GetSide()
				<< "] Price [" << dOrderPrice
				<< "] Quantity [" << nOrderQty
				<< "] Dest [" << pszDestination
				<< "] ErrorCode [" << ErrorCodeToStr(eReturn)
				<< "]"
				<< std::endl;
		return FAILURE;
	}
    else
    {
		CINFO << "Successfully sent street order : Client OrderID [" << ClientOrder.GetHandle()
				<< "] Street Order ID [" << order.GetHandle()
				<< "] Symbol [" << ClientOrder.GetSymbol()
				<< "] Portfolio [" << ClientOrder.GetPortfolio()
				<< "] Side [" << ClientOrder.GetSide()
				<< "] LegId [" << ClientOrder.GetFixTag(FIX_TAG_LEGID_BO)
				<< "] Price [" << dOrderPrice
				<< "] Quantity [" << nOrderQty
				<< "] Dest [" << pszDestination
				<< "] Wave [" << order.GetWaveName()
				<< "] ClientId2 [" << order.GetClientId2()
				<< "]"
				<< std::endl;
    	return SUCCESS;
    }
}

/// UTILITY FUNCTION: Stop the strategy when limit has been breached
int BidOfferSpread::StopStratOnRiskLimitBreached(StratParams* pStratParams)
{
	CWARN << "Stopping StratID [" << pStratParams->nStratId << "] !" << std::endl;

	// Step 1 : STOP strategy
	pStratParams->bStratRunning = false;

	PrintStratOrderMap(gmStratMap, ((long)pStratParams->nStratId));

	// Step 2: Send cancels on all street orders
	CancelAllStreetOrders(pStratParams);

	return SUCCESS;
}

/// UTILITY FUNCTION: Cancel all street orders for a given client order
int BidOfferSpread::CancelAllOrdersForClientOrder(const char* pszClientOrderId)
{
    static STREET_ORDER_CONTAINER container;
    static STREET_ORDER TmpStreetOrder;
    if(!pszClientOrderId)
    	return FAILURE;

    CINFO << "Cancelling All Street orders for ClientOrderId [" << pszClientOrderId << "]" << std::endl;

    if(container.GetFirstActiveByClientOrderId(TmpStreetOrder, pszClientOrderId))
    {
    	do
    	{
    		if(!CanReplace(TmpStreetOrder))
    		{
    			CWARN << "Cannot Cancel Street Order [" << TmpStreetOrder.GetHandle()
    					<<"] , OrderState [" << StateToStr(TmpStreetOrder.GetOrderState())
    					<<"]"
    					<< std::endl;

    			_STRAT_LOG_VERB_(PrintSHFEOrdRplMap(gmSHFEOrdRplMap));

    			SHFEORDERRPLMAPITER mShfeRplIter = gmSHFEOrdRplMap.find(string(TmpStreetOrder.GetHandle()));

    			if (mShfeRplIter != gmSHFEOrdRplMap.end())
				{
					CWARN << "StreetOrder ID [" << TmpStreetOrder.GetHandle()
							<< "] Symbol [" << TmpStreetOrder.GetSymbol()
							<< "] Side [" << TmpStreetOrder.GetSide()
							<< "] is Pending Replace in gmSHFEOrdRplMap : Remove Order [" << TmpStreetOrder.GetHandle()
							<< "] from gmSHFEOrdRplMap and discard replace order."
							<< std::endl;

					gmSHFEOrdRplMap.erase(mShfeRplIter);

					_STRAT_LOG_DBUG_(PrintSHFEOrdRplMap(gmSHFEOrdRplMap));
				}

    			continue;
    		}

    		TmpStreetOrder.SetClientOrdId(pszClientOrderId);

    		enErrorCode errStreetCancel = TmpStreetOrder.Cancel();
    		if(errStreetCancel != RULES_EO_OK)
    		{
    			CERR << "Cancel Fail for [" << TmpStreetOrder.GetHandle()
    					<<"] errStreetCancel [" << ErrorCodeToStr(errStreetCancel)
    					<<"]"
    					<< std::endl;
    		}
    		else
    		{
    			CINFO << "Cancel Success for [" << TmpStreetOrder.GetHandle() << "]" << std::endl;
    		}
    	}
    	while(container.GetNextActiveByClientOrderId(TmpStreetOrder));
    }
    return SUCCESS;
}

/// UTILITY FUNCTION: Cancel excess street orders for a given client order, to achieve working size nNetStratExecs
int	BidOfferSpread::CancelExcessOrderForClientOrder(const char* pszClientOrderId, int nNetStratExecs)
{
	STREET_ORDER_CONTAINER StreetOrderContainer;
	STREET_ORDER TmpStreetOrder;
	CLIENT_ORDER_CONTAINER ClientOrderContainer;
	CLIENT_ORDER ClientOrder;
	SORTED_ORDERS_MULTIMAP StreetOrderMultimap;

	if(!pszClientOrderId)
	{
		_STRAT_LOG_VERB_(CERR << "Invalid ClientOrderID!" << std::endl);
		return FAILURE;
	}

	if (!ClientOrderContainer.GetActiveByClientOrderId(ClientOrder, pszClientOrderId))
	{
		_STRAT_LOG_VERB_(CERR << "Failed to find active client order by ClientOrderID [" <<  pszClientOrderId
				<< "]!"
				<< std::endl);
		return FAILURE;
	}

	CWARN << "Reducing ClientOrderId [" << pszClientOrderId
			<< "] Side [" << ClientOrder.GetSide()
			<< "] Working Size from [" << ClientOrder.GetWorkingSize()
			<< "] to [" <<  nNetStratExecs
			<< "] by cancelling excess orders!"
			<< std::endl;

	// Step 1 : Sort street orders by price
	// Buy orders: sorted by price negated
	// Sell orders: sorted by price
	if(StreetOrderContainer.GetFirstActiveByClientOrderId(TmpStreetOrder, pszClientOrderId))
	{
		do
		{
			_STRAT_LOG_DBUG_(CDEBUG << "ClientOrder ID [" << pszClientOrderId
					<<"] found StreetOrder ID [" << TmpStreetOrder.GetHandle()
					<<"] Side [" << TmpStreetOrder.GetSide()
					<<"] Price [" << TmpStreetOrder.GetPrice()
					<<"] Qty [" << TmpStreetOrder.GetSize()
					<<"] - Inserting into StreetOrderMultimap."
					<< std::endl);

			if (TmpStreetOrder.GetSide() == SIDE_BUY)
			{
				StreetOrderMultimap.insert(pair<double, STREET_ORDER>(TmpStreetOrder.GetPrice()*(-1.0), TmpStreetOrder));
			}
			else if (TmpStreetOrder.GetSide() == SIDE_SELL)

			{
				StreetOrderMultimap.insert(pair<double, STREET_ORDER>(TmpStreetOrder.GetPrice(), TmpStreetOrder));
			}
			else
			{
				_STRAT_LOG_VERB_(CERR << "ClientOrder ID [" << pszClientOrderId
						<<"] found StreetOrder ID [" << TmpStreetOrder.GetHandle()
						<<"] Side [" << TmpStreetOrder.GetSide()
						<<"] Price [" << TmpStreetOrder.GetPrice()
						<<"] Qty [" << TmpStreetOrder.GetSize()
						<<"] - Invalid Side!"
						<< std::endl);
			}
		}
		while(StreetOrderContainer.GetNextActiveByClientOrderId(TmpStreetOrder));
	}

	_STRAT_LOG_VERB_(PrintSortedOrdersMultimap(StreetOrderMultimap));


	// Step 2 : Send Cancel Requests on excess orders
	int nMinToCancelQty = 0, nCancelReqQty = 0;
	nMinToCancelQty = ClientOrder.GetWorkingSize() - nNetStratExecs;

	CINFO << "Hedge ClientOrder ID [" << ClientOrder.GetHandle()
			<< "] Side [" << ClientOrder.GetSide()
			<< "] WorkingSize [" << ClientOrder.GetWorkingSize()
			<< "] vs NetStratExecs [" << nNetStratExecs
			<< "] -> Need to cancel MinToCancelQty [" << nMinToCancelQty
			<< "]!"
			<< std::endl;

	SORTED_ORDERS_MULTIMAP_REVITER mSortedOrdIter;
	mSortedOrdIter = StreetOrderMultimap.rbegin();

	_STRAT_LOG_DBUG_(CDEBUG << "Iterating through StreetOrderMultimap!"
							<< " : Begin with StreetOrder Price =[" <<  (*mSortedOrdIter).first
							<< "] | OrderID =[" << ((*mSortedOrdIter).second).GetHandle()
							<< "] | Symbol =[" <<  ((*mSortedOrdIter).second).GetSymbol()
							<< "] | Side =[" << ((*mSortedOrdIter).second).GetSide()
							<< "] | Qty =[" << ((*mSortedOrdIter).second).GetSize()
							<< "]"
							<< std::endl);

	while (nCancelReqQty < nMinToCancelQty && mSortedOrdIter!=StreetOrderMultimap.rend())
	{
		TmpStreetOrder =  (STREET_ORDER) mSortedOrdIter->second;

		_STRAT_LOG_VERB_(CWARN << "ClientOrder ID [" << pszClientOrderId
								<< "] CancelReqQty [" << nCancelReqQty
								<< "] MinToCancelQty [" << nMinToCancelQty
								<< "] : Processing StreetOrder ID [" << TmpStreetOrder.GetHandle()
								<<"]"
								<< std::endl);

		if(!CanReplace(TmpStreetOrder))
		{
			_STRAT_LOG_VERB_(CWARN << "Cannot Cancel Street Order [" << TmpStreetOrder.GetHandle()
					<<"] , OrderState [" << StateToStr(TmpStreetOrder.GetOrderState())
					<<"]"
					<< std::endl);

			if (TmpStreetOrder.isCancelPending() || TmpStreetOrder.isReplacePending())
			{
				nCancelReqQty += TmpStreetOrder.GetSize();
			}

			mSortedOrdIter++;
			continue;
		}

		int nToCancelQty = 0;
		nToCancelQty = TmpStreetOrder.GetSize();

		if (nToCancelQty + nCancelReqQty > nMinToCancelQty)
		{
			// Replace
			int nToRplQty = 0;
			nToRplQty = nMinToCancelQty - nCancelReqQty;

			CINFO << "CancelReqQty [" << nCancelReqQty
					<< "] vs MinToCancelQty [" << nMinToCancelQty
					<< "] : To-Cancel StreetOrder ID [" << TmpStreetOrder.GetHandle()
					<< "] Side [" << TmpStreetOrder.GetSide()
					<< "] Quantity [" << TmpStreetOrder.GetSize()
					<< "] Price [" << TmpStreetOrder.GetPrice()
					<< "] -> Replacing to Qty [" << nToRplQty
					<< "]!"
					<< std::endl;

			if (HandleSHFEReplace(TmpStreetOrder, nToRplQty, TmpStreetOrder.GetPrice())==SUCCESS)
			{
				nCancelReqQty = nMinToCancelQty;
			}
			else
			{
				_STRAT_LOG_VERB_(CWARN << "SHFE Replace Failed for Street Order [" << TmpStreetOrder.GetHandle()
						<<"] , OrderState [" << StateToStr(TmpStreetOrder.GetOrderState())
						<<"]"
						<< std::endl);
			}
		}
		else
		{
			// Cancel
			CINFO << "CancelReqQty [" << nCancelReqQty
					<< "] vs MinToCancelQty [" << nMinToCancelQty
					<< "] : To-Cancel StreetOrder ID [" << TmpStreetOrder.GetHandle()
					<< "] Side [" << TmpStreetOrder.GetSide()
					<< "] Quantity [" << TmpStreetOrder.GetSize()
					<< "] Price [" << TmpStreetOrder.GetPrice()
					<< "] -> Cancelling!"
					<< std::endl;

			enErrorCode errStreetCancel = TmpStreetOrder.Cancel();
			if(errStreetCancel != RULES_EO_OK)
			{
				CERR << "Cancel Fail for [" << TmpStreetOrder.GetHandle()
						<<"] errStreetCancel [" << ErrorCodeToStr(errStreetCancel)
						<<"]"
						<< std::endl;
			}
			else
			{
				nCancelReqQty += nToCancelQty;
				CINFO << "Cancel Success for StreetOrderID [" << TmpStreetOrder.GetHandle()
						<< "] with Qty [" << nToCancelQty
						<< "] - MinToCancelQty vs CancelReqQty [" << nMinToCancelQty << "/" << nCancelReqQty
						<< "]"
						<< std::endl;
			}
		}

		mSortedOrdIter++;
	}

	return SUCCESS;

}

// UTILITY FUNCTION: Can we modify the existing street order?
bool BidOfferSpread::CanReplace(STREET_ORDER& StreetOrder)
{
    if(StreetOrder.isReplacePending())
    {
    	_STRAT_LOG_VERB_(CWARN << "Street Order [" << StreetOrder.GetHandle()
    							<<"] Cannot Rpl/Cxl : Pending Replace! "
    							<< std::endl);
    	return false;
    }

    if(StreetOrder.isCancelPending())
	{
		_STRAT_LOG_VERB_(CWARN << "Street Order [" << StreetOrder.GetHandle()
								<<"] Cannot Rpl/Cxl : Pending Cancel! "
								<< std::endl);
		return false;
	}

    if(StreetOrder.GetOrderType() == TYPE_MARKET)
    {
		_STRAT_LOG_VERB_(CWARN << "Street Order [" << StreetOrder.GetHandle()
								<<"] Cannot Rpl: Market Order! "
								<< std::endl);
		return false;
    }

    if(StreetOrder.GetOrderTimeInForce() == TIF_IOC)
    {
    	_STRAT_LOG_VERB_(CWARN << "Street Order [" << StreetOrder.GetHandle()
    							<<"] Cannot Rpl: Market Order! "
    							<< std::endl);
    	return false;
    }

    switch(StreetOrder.GetOrderState())
    {
		case STATE_OPEN:
		case STATE_PARTIAL:
		case STATE_REPLACED:
			return true;
		case STATE_UNACKED:
		case STATE_FILLED:
		case STATE_CANCELLED:
		case STATE_PENDING_RPL:
		case STATE_REJECTED:
		case STATE_DONE:
		case STATE_INVALID:
		default:
			return false;
    }
    return false;
}

/// UTILITY FUNCTION: Return order state
const char* BidOfferSpread::StateToStr(enOrderState e)
{
    static char *p[] = {"OPEN", "PART", "RPLD", "UNACKED", "FILLED", "CXLD", "PEND-RPL", "REJD", "DONE", "PEND-NEW", "INVALID"};
    switch(e)
    {
		case STATE_OPEN:
			return p[0];
		case STATE_PARTIAL:
			return p[1];
		case STATE_REPLACED:
			return p[2];
		case STATE_UNACKED:
			return p[3];
		case STATE_FILLED:
			return p[4];
		case STATE_CANCELLED:
			return p[5];
		case STATE_PENDING_RPL:
			return p[6];
		case STATE_REJECTED:
			return p[7];
		case STATE_DONE:
			return p[8];
		case STATE_PENDING_NEW:
			return p[9];
		case STATE_INVALID:
		default:
			return p[10];
    }
    return p[10];
}

/// UTILITY FUNCTION: Return Strat mode
const char* BidOfferSpread::StratModeToStr(E_STRAT_MODE e)
{
	static char *p[] = {"INVALID",
						"ALWAYS",
						"CONDITIONAL",
						"AGGRESSIVE"};

	switch(e)
	{
		case STRAT_MODE_INVALID:
			return p[0];
		case STRAT_MODE_ALWAYS:
			return p[1];
		case STRAT_MODE_CONDITIONAL:
			return p[2];
		case STRAT_MODE_AGGRESSIVE:
			return p[3];
	};
	return p[0];
}

/// UTILITY FUNCTION: Return error code
const char* BidOfferSpread::ErrorCodeToStr(enErrorCode e)
{
	static char *p[] = { "RULES_EO_OK",
						 "RULES_EO_FAIL",
						 "RULES_EO_NO_LVS",
						 "RULES_EO_NO_ORD",
						 "RULES_EO_NO_PORT",
						 "RULES_EO_OTHER_USER",
						 "RULES_EO_MAX_REJ",
						 "RULES_EO_NO_CLIENT_ORD",
						 "RULES_EO_NOT_ACTIVE",
						 "RULES_EO_REF_CXLD",
						 "RULES_EO_REF_RPLD",
						 "RULES_EO_INVALID_PRICE",
						 "RULES_EO_EXCEED_UNORD_SHRS",
						 "RULES_EO_DEST_NOT_UP",
						 "RULES_EO_SOCKET_FAIL" };
	switch (e)
	{
		case RULES_EO_OK:
			return p[0];
		case RULES_EO_FAIL:
			return p[1];
		case RULES_EO_NO_LVS:
			return p[2];
		case RULES_EO_NO_ORD:
			return p[3];
		case RULES_EO_NO_PORT:
			return p[4];
		case RULES_EO_OTHER_USER:
			return p[5];
		case RULES_EO_MAX_REJ:
			return p[6];
		case RULES_EO_NO_CLIENT_ORD:
			return p[7];
		case RULES_EO_NOT_ACTIVE:
			return p[8];
		case RULES_EO_REF_CXLD:
			return p[9];
		case RULES_EO_REF_RPLD:
			return p[10];
		case RULES_EO_INVALID_PRICE:
			return p[11];
		case RULES_EO_EXCEED_UNORD_SHRS:
			return p[12];
		case RULES_EO_DEST_NOT_UP:
			return p[13];
		case RULES_EO_SOCKET_FAIL:
			return p[14];
	}
	return p[1];
}

/// UTILITY FUNCTION: Get the default tick size for different symbols
double	BidOfferSpread::GetTickSizeForSymbol(const char* pszSymbol)
{
    if(!pszSymbol)
    {
    	CERR <<"Invalid Symbol ["<< pszSymbol << "]" << std::endl;
    	return g_dEpsilon/10.0;
    }

    if(strstr(pszSymbol, "AU"))
    	return 0.05;
    else if(strstr(pszSymbol, "CU"))
    	return 10.0;
    else if(strstr(pszSymbol, "AG"))
    	return 1.0;
    else if(strstr(pszSymbol, "IF"))
    	return 0.2;

    return g_dEpsilon/10.0;
}

/// UTILITY FUNCTION: Get the default contract size for different symbols
int	BidOfferSpread::GetContractSizeForSymbol(const char* pszSymbol)
{
    if(!pszSymbol)
    {
    	CERR <<"Invalid Symbol ["<< pszSymbol << "]" << std::endl;
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

/// UTILITY FUNCTION: Get the default unit price for different symbols
double BidOfferSpread::GetContractUnitPriceForSymbol(const char* pszSymbol)
{
    if(!pszSymbol)
    {
    	CERR <<"Invalid Symbol ["<< pszSymbol << "]" << std::endl;
    	return 0;
    }

    return double (GetContractSizeForSymbol(pszSymbol)) ;
}


/// UTILITY FUNCTION: Cancels all street orders of the strat
int BidOfferSpread::CancelAllStreetOrders(const StratParams *pStratParams)
{
	if (!pStratParams)
	{
		_STRAT_LOG_DBUG_(CDEBUG << "Invalid pStratParams!" << std::endl);
		return FAILURE;
	}

	_STRAT_LOG_VERB_(CWARN << "StratID [" << pStratParams->nStratId
					<<"] Sending cancel!"
					<< std::endl);

	if ((CancelAllOrdersForClientOrder(pStratParams->szClientOrdIdBidQuote) == SUCCESS)
			&& (CancelAllOrdersForClientOrder(pStratParams->szClientOrdIdBidHedge) == SUCCESS)
			&& (CancelAllOrdersForClientOrder(pStratParams->szClientOrdIdOfferQuote) == SUCCESS)
			&& (CancelAllOrdersForClientOrder(pStratParams->szClientOrdIdOfferHedge) == SUCCESS))
		return SUCCESS;
	else
		return FAILURE;
}

/// UTILITY FUNCTION: Prints Client Order Map
void BidOfferSpread::PrintStratOrderMap(STRATMAP& mpStratMap, const long nStratId)
{
	CDEBUG << "Printing StratMap : StratID [" << nStratId << "]." << std::endl;
	if(nStratId < 1)
	{
		CDEBUG << "No StratID [" << nStratId
				<< "] passed; hence printing all." << std::endl;
	}
	if (mpStratMap.count(nStratId) <= 0)
	{
		CDEBUG << "StratID [" << nStratId
				<< "] is not found in the map!" << std::endl;
	}
	if(mpStratMap.count(nStratId))
	{
		STRATMAPITER miIter =  mpStratMap.find(nStratId);
		if (miIter != mpStratMap.end())
		{
			CDEBUG << "\n New Strat Order"
				<< "| nStratId=" << miIter->second->nStratId
				<< "| szStratPort=" << miIter->second->szStratPort
				<< "| eStratMode=" << StratModeToStr(miIter->second->eStratMode)
				<< "| bStratReady=" << miIter->second->bStratReady
				<< "| bStratRunning=" << miIter->second->bStratRunning
				<< "| szSymbolQuote=" << miIter->second->szSymbolQuote
				<< "| nLotSizeQuote=" << miIter->second->nLotSizeQuote
				<< "| szSymbolHedge=" << miIter->second->szSymbolHedge
				<< "| nLotSizeHedge=" << miIter->second->nLotSizeHedge
				<< "| szOMUser=" << miIter->second->szOMUser
				<< "| szDest=" << miIter->second->szDest
				<< "| nQtyPerOrder="  << miIter->second->nQtyPerOrder
				<< "| nPayUpTicks=" <<  miIter->second->nPayUpTicks
				<< "| nMaxPosition=" << miIter->second->nMaxPosition
				<< "| nMaxRplLimit=" << miIter->second->nMaxRplLimit
				<< "| nQuoteRplCount=" << miIter->second->nQuoteRplCount
				<< "| nHedgeRplCount=" << miIter->second->nHedgeRplCount
				<< "| dMaxLoss=" << miIter->second->dMaxLoss
				<< "| nNumStages=" << miIter->second->nNumStages
				<< "| nSpreadInterval=" << miIter->second->nSpreadInterval
				<< "| enSideBidQuote=" << miIter->second->enSideBidQuote
				<< "| enSideBidHedge=" << miIter->second->enSideBidHedge
				<< "| enSideOfferQuote=" << miIter->second->enSideOfferQuote
				<< "| enSideOfferHedge=" << miIter->second->enSideOfferHedge
				<< " \n Bid:  "
				<< "| dQuoteLastBotPrice=" <<  miIter->second->dQuoteLastBotPrice
				<< "| nQuoteLastBotQty=" << miIter->second->nQuoteLastBotQty
				<< "| bBidQuoteEntry=" <<  miIter->second->bBidQuoteEntry
				<< "| bBidHedgeEntry=" <<  miIter->second->bBidHedgeEntry
				<< "| bBidQuoteRpld=" << miIter->second->bBidQuoteRpld
				<< "| bBidHedgeRpld=" <<  miIter->second->bBidHedgeRpld
				<< "| szPortfolioBid=" << miIter->second->szPortfolioBid
				<< "| dBenchmarkBid=" << miIter->second->dBenchmarkBid
				<< "| dStateBidBench=" << miIter->second->dStateBidBench
				<< "| szClientOrdIdBidQuote=" << miIter->second->szClientOrdIdBidQuote
				<< "| szClientOrdIdBidHedge=" << miIter->second->szClientOrdIdBidHedge
				<< " \n Offer:  "
				<< "| dQuoteLastSoldPrice=" <<  miIter->second->dQuoteLastSoldPrice
				<< "| nQuoteLastSoldQty=" << miIter->second->nQuoteLastSoldQty
				<< "| bOfferQuoteEntry=" <<  miIter->second->bOfferQuoteEntry
				<< "| bOfferHedgeEntry=" <<  miIter->second->bOfferHedgeEntry
				<< "| bOfferQuoteRpld=" <<  miIter->second->bOfferQuoteRpld
				<< "| bOfferHedgeRpld=" <<  miIter->second->bOfferHedgeRpld
				<< "| szPortfolioOffer=" << miIter->second->szPortfolioOffer
				<< "| dBenchmarkOffer=" << miIter->second->dBenchmarkOffer
				<< "| dStateOfferBench=" << miIter->second->dStateOfferBench
				<< "| szClientOrdIdOfferQuote=" << miIter->second->szClientOrdIdOfferQuote
				<< "| szClientOrdIdOfferHedge=" << miIter->second->szClientOrdIdOfferHedge
				<< "\n"
				<< std::endl;
		}
	}
	else
	{
		STRATMAPITER miIter;
		for(miIter = mpStratMap.begin(); miIter != mpStratMap.end() ; miIter++)
		{
			CDEBUG << "\n New Strat Order"
				<< "| nStratId=" << miIter->second->nStratId
				<< "| szStratPort=" << miIter->second->szStratPort
				<< "| eStratMode=" << StratModeToStr(miIter->second->eStratMode)
				<< "| bStratReady=" << miIter->second->bStratReady
				<< "| bStratRunning=" << miIter->second->bStratRunning
				<< "| szSymbolQuote=" << miIter->second->szSymbolQuote
				<< "| nLotSizeQuote=" << miIter->second->nLotSizeQuote
				<< "| szSymbolHedge=" << miIter->second->szSymbolHedge
				<< "| nLotSizeHedge=" << miIter->second->nLotSizeHedge
				<< "| szOMUser=" << miIter->second->szOMUser
				<< "| szDest=" << miIter->second->szDest
				<< "| nQtyPerOrder="  << miIter->second->nQtyPerOrder
				<< "| nPayUpTicks=" <<  miIter->second->nPayUpTicks
				<< "| nMaxPosition=" << miIter->second->nMaxPosition
				<< "| nMaxRplLimit=" << miIter->second->nMaxRplLimit
				<< "| nQuoteRplCount=" << miIter->second->nQuoteRplCount
				<< "| nHedgeRplCount=" << miIter->second->nHedgeRplCount
				<< "| dMaxLoss=" << miIter->second->dMaxLoss
				<< "| nNumStages=" << miIter->second->nNumStages
				<< "| nSpreadInterval=" << miIter->second->nSpreadInterval
				<< "| enSideBidQuote=" << miIter->second->enSideBidQuote
				<< "| enSideBidHedge=" << miIter->second->enSideBidHedge
				<< "| enSideOfferQuote=" << miIter->second->enSideOfferQuote
				<< "| enSideOfferHedge=" << miIter->second->enSideOfferHedge
				<< " \n Bid:  "
				<< "| dQuoteLastBotPrice=" <<  miIter->second->dQuoteLastBotPrice
				<< "| nQuoteLastBotQty=" << miIter->second->nQuoteLastBotQty
				<< "| bBidQuoteEntry=" <<  miIter->second->bBidQuoteEntry
				<< "| bBidHedgeEntry=" <<  miIter->second->bBidHedgeEntry
				<< "| bBidQuoteRpld=" << miIter->second->bBidQuoteRpld
				<< "| bBidHedgeRpld=" <<  miIter->second->bBidHedgeRpld
				<< "| szPortfolioBid=" << miIter->second->szPortfolioBid
				<< "| dBenchmarkBid=" << miIter->second->dBenchmarkBid
				<< "| dStateBidBench=" << miIter->second->dStateBidBench
				<< "| szClientOrdIdBidQuote=" << miIter->second->szClientOrdIdBidQuote
				<< "| szClientOrdIdBidHedge=" << miIter->second->szClientOrdIdBidHedge
				<< " \n Offer:  "
				<< "| dQuoteLastSoldPrice=" <<  miIter->second->dQuoteLastSoldPrice
				<< "| nQuoteLastSoldQty=" << miIter->second->nQuoteLastSoldQty
				<< "| bOfferQuoteEntry=" <<  miIter->second->bOfferQuoteEntry
				<< "| bOfferHedgeEntry=" <<  miIter->second->bOfferHedgeEntry
				<< "| bOfferQuoteRpld=" <<  miIter->second->bOfferQuoteRpld
				<< "| bOfferHedgeRpld=" <<  miIter->second->bOfferHedgeRpld
				<< "| szPortfolioOffer=" << miIter->second->szPortfolioOffer
				<< "| dBenchmarkOffer=" << miIter->second->dBenchmarkOffer
				<< "| dStateOfferBench=" << miIter->second->dStateOfferBench
				<< "| szClientOrdIdOfferQuote=" << miIter->second->szClientOrdIdOfferQuote
				<< "| szClientOrdIdOfferHedge=" << miIter->second->szClientOrdIdOfferHedge
				<< "\n"
				<< std::endl;
		}
	}
}

/// UTILITY FUNCTION: Prints Client Order Rpl Map
void BidOfferSpread::PrintStratOrderMapRpl(STRATMAP& mpStratMapRpl)
{
	CDEBUG << "Printing StratOrderMapRpl - printing all Rpl orders." << std::endl;
	STRATMAPITER miIter;
	for(miIter = mpStratMapRpl.begin(); miIter != mpStratMapRpl.end() ; miIter++)
	{
		CDEBUG << "\n Rpl Strat Order"
				<< "| nStratId=" << miIter->second->nStratId
				<< "| szStratPort=" << miIter->second->szStratPort
				<< "| eStratMode=" << StratModeToStr(miIter->second->eStratMode)
				<< "| bStratReady=" << miIter->second->bStratReady
				<< "| bStratRunning=" << miIter->second->bStratRunning
				<< "| szSymbolQuote=" << miIter->second->szSymbolQuote
				<< "| nLotSizeQuote=" << miIter->second->nLotSizeQuote
				<< "| szSymbolHedge=" << miIter->second->szSymbolHedge
				<< "| nLotSizeHedge=" << miIter->second->nLotSizeHedge
				<< "| szOMUser=" << miIter->second->szOMUser
				<< "| szDest=" << miIter->second->szDest
				<< "| nQtyPerOrder="  << miIter->second->nQtyPerOrder
				<< "| nPayUpTicks=" <<  miIter->second->nPayUpTicks
				<< "| nMaxPosition=" << miIter->second->nMaxPosition
				<< "| nMaxRplLimit=" << miIter->second->nMaxRplLimit
				<< "| nQuoteRplCount=" << miIter->second->nQuoteRplCount
				<< "| nHedgeRplCount=" << miIter->second->nHedgeRplCount
				<< "| dMaxLoss=" << miIter->second->dMaxLoss
				<< "| nNumStages=" << miIter->second->nNumStages
				<< "| nSpreadInterval=" << miIter->second->nSpreadInterval
				<< "| enSideBidQuote=" << miIter->second->enSideBidQuote
				<< "| enSideBidHedge=" << miIter->second->enSideBidHedge
				<< "| enSideOfferQuote=" << miIter->second->enSideOfferQuote
				<< "| enSideOfferHedge=" << miIter->second->enSideOfferHedge
				<< " \n Bid:  "
				<< "| dQuoteLastBotPrice=" <<  miIter->second->dQuoteLastBotPrice
				<< "| nQuoteLastBotQty=" << miIter->second->nQuoteLastBotQty
				<< "| bBidQuoteEntry=" <<  miIter->second->bBidQuoteEntry
				<< "| bBidHedgeEntry=" <<  miIter->second->bBidHedgeEntry
				<< "| bBidQuoteRpld=" << miIter->second->bBidQuoteRpld
				<< "| bBidHedgeRpld=" <<  miIter->second->bBidHedgeRpld
				<< "| szPortfolioBid=" << miIter->second->szPortfolioBid
				<< "| dBenchmarkBid=" << miIter->second->dBenchmarkBid
				<< "| dStateBidBench=" << miIter->second->dStateBidBench
				<< "| szClientOrdIdBidQuote=" << miIter->second->szClientOrdIdBidQuote
				<< "| szClientOrdIdBidHedge=" << miIter->second->szClientOrdIdBidHedge
				<< " \n Offer:  "
				<< "| dQuoteLastSoldPrice=" <<  miIter->second->dQuoteLastSoldPrice
				<< "| nQuoteLastSoldQty=" << miIter->second->nQuoteLastSoldQty
				<< "| bOfferQuoteEntry=" <<  miIter->second->bOfferQuoteEntry
				<< "| bOfferHedgeEntry=" <<  miIter->second->bOfferHedgeEntry
				<< "| bOfferQuoteRpld=" <<  miIter->second->bOfferQuoteRpld
				<< "| bOfferHedgeRpld=" <<  miIter->second->bOfferHedgeRpld
				<< "| szPortfolioOffer=" << miIter->second->szPortfolioOffer
				<< "| dBenchmarkOffer=" << miIter->second->dBenchmarkOffer
				<< "| dStateOfferBench=" << miIter->second->dStateOfferBench
				<< "| szClientOrdIdOfferQuote=" << miIter->second->szClientOrdIdOfferQuote
				<< "| szClientOrdIdOfferHedge=" << miIter->second->szClientOrdIdOfferHedge
				<< "\n"
				<< std::endl;
	}
}

/// UTILITY FUNCTION: Prints Client Order Cxl Map
void BidOfferSpread::PrintStratOrderMapCxl(STRATMAP& mpStratMapCxl)
{
	CDEBUG << "Printing StratOrderMapCxl - printing all Cxl orders." << std::endl;
	STRATMAPITER miIter;
	for(miIter = mpStratMapCxl.begin(); miIter != mpStratMapCxl.end() ; miIter++)
	{
		CDEBUG << "\n Cxl Strat Order"
				<< "| nStratId=" << miIter->second->nStratId
				<< "| szStratPort=" << miIter->second->szStratPort
				<< "| eStratMode=" << StratModeToStr(miIter->second->eStratMode)
				<< "| bStratReady=" << miIter->second->bStratReady
				<< "| bStratRunning=" << miIter->second->bStratRunning
				<< "| szSymbolQuote=" << miIter->second->szSymbolQuote
				<< "| nLotSizeQuote=" << miIter->second->nLotSizeQuote
				<< "| szSymbolHedge=" << miIter->second->szSymbolHedge
				<< "| nLotSizeHedge=" << miIter->second->nLotSizeHedge
				<< "| szOMUser=" << miIter->second->szOMUser
				<< "| szDest=" << miIter->second->szDest
				<< "| nQtyPerOrder="  << miIter->second->nQtyPerOrder
				<< "| nPayUpTicks=" <<  miIter->second->nPayUpTicks
				<< "| nMaxPosition=" << miIter->second->nMaxPosition
				<< "| nMaxRplLimit=" << miIter->second->nMaxRplLimit
				<< "| nQuoteRplCount=" << miIter->second->nQuoteRplCount
				<< "| nHedgeRplCount=" << miIter->second->nHedgeRplCount
				<< "| dMaxLoss=" << miIter->second->dMaxLoss
				<< "| nNumStages=" << miIter->second->nNumStages
				<< "| nSpreadInterval=" << miIter->second->nSpreadInterval
				<< "| enSideBidQuote=" << miIter->second->enSideBidQuote
				<< "| enSideBidHedge=" << miIter->second->enSideBidHedge
				<< "| enSideOfferQuote=" << miIter->second->enSideOfferQuote
				<< "| enSideOfferHedge=" << miIter->second->enSideOfferHedge
				<< " \n Bid:  "
				<< "| dQuoteLastBotPrice=" <<  miIter->second->dQuoteLastBotPrice
				<< "| nQuoteLastBotQty=" << miIter->second->nQuoteLastBotQty
				<< "| bBidQuoteEntry=" <<  miIter->second->bBidQuoteEntry
				<< "| bBidHedgeEntry=" <<  miIter->second->bBidHedgeEntry
				<< "| bBidQuoteRpld=" << miIter->second->bBidQuoteRpld
				<< "| bBidHedgeRpld=" <<  miIter->second->bBidHedgeRpld
				<< "| szPortfolioBid=" << miIter->second->szPortfolioBid
				<< "| dBenchmarkBid=" << miIter->second->dBenchmarkBid
				<< "| dStateBidBench=" << miIter->second->dStateBidBench
				<< "| szClientOrdIdBidQuote=" << miIter->second->szClientOrdIdBidQuote
				<< "| szClientOrdIdBidHedge=" << miIter->second->szClientOrdIdBidHedge
				<< " \n Offer:  "
				<< "| dQuoteLastSoldPrice=" <<  miIter->second->dQuoteLastSoldPrice
				<< "| nQuoteLastSoldQty=" << miIter->second->nQuoteLastSoldQty
				<< "| bOfferQuoteEntry=" <<  miIter->second->bOfferQuoteEntry
				<< "| bOfferHedgeEntry=" <<  miIter->second->bOfferHedgeEntry
				<< "| bOfferQuoteRpld=" <<  miIter->second->bOfferQuoteRpld
				<< "| bOfferHedgeRpld=" <<  miIter->second->bOfferHedgeRpld
				<< "| szPortfolioOffer=" << miIter->second->szPortfolioOffer
				<< "| dBenchmarkOffer=" << miIter->second->dBenchmarkOffer
				<< "| dStateOfferBench=" << miIter->second->dStateOfferBench
				<< "| szClientOrdIdOfferQuote=" << miIter->second->szClientOrdIdOfferQuote
				<< "| szClientOrdIdOfferHedge=" << miIter->second->szClientOrdIdOfferHedge
				<< "\n"
				<< std::endl;
	}
}

/// UTILITY FUNCTION: Prints Information from Street execution report
void BidOfferSpread::PrintStreetExec(STREET_EXEC& StreetExec, CLIENT_ORDER& parentOrder)
{
	CINFO << "StreetOrder :"
			<< " StreetOrder Handle=[" <<  (StreetExec.GetStreetOrder()).GetHandle()
			<< "] | StreetOrder State [" << StateToStr((StreetExec.GetStreetOrder()).GetOrderState())
			<< "] | StreetOrder GetClientId0=[" << (StreetExec.GetStreetOrder()).GetClientId0()
			<< "] | StreetOrder GetClientId2=[" << (StreetExec.GetStreetOrder()).GetClientId2()
			<< "] - StreetExecs : GetClientOrdId=[" << StreetExec.GetClientOrdId()
			<< "] | GetSymbol=[" << StreetExec.GetSymbol()
			<< "] | GetLastFillQty=[" << StreetExec.GetLastFillQty()
			<< "] | GetLastFillPrice=[" << StreetExec.GetLastFillPrice()
			<< "] | GetFilledSize=[" << StreetExec.GetFilledSize()
			<< "] | GetSize=[" << StreetExec.GetSize()
			<< "] | GetPrice=[" << StreetExec.GetPrice()
			<< "] | GetTotalLvs=[" << StreetExec.GetTotalLvs()
			<< "] | GetOrderState=[" << StateToStr(StreetExec.GetOrderState())
			<< "] | GetSide=[" << StreetExec.GetSide()
			<< "] | GetExecBroker=[" << StreetExec.GetExecBroker()
			<< "] | GetBrokerOrdId=[" << StreetExec.GetBrokerOrdId()
			<< "] | GetCapacity=[" << StreetExec.GetCapacity()
			<< "] | GetTradingAccount=[" << StreetExec.GetTradingAccount()
			<< "] | GetExecId=[" << StreetExec.GetExecId()
			<< "] | GetExecRefId=[" << StreetExec.GetExecRefId()
			<< "] | GetHandle=[" << StreetExec.GetHandle()
			<< "] | GetDestination=[" << StreetExec.GetDestination()
			<< "] | GetDestinationSubId=[" << StreetExec.GetDestinationSubId()
			<< "] | GetSender=[" << StreetExec.GetSender()
			<< "] | GetSenderSubId=[" << StreetExec.GetSenderSubId()
			<< "] | GetClientId2=[" << StreetExec.GetClientId2()
			<< "] | GetTradeDate=[" << StreetExec.GetTradeDate()
			<< "]"
			<< std::endl;
}

/// UTILITY FUNCTION: Prints SHFE replace map
void BidOfferSpread::PrintSHFEOrdRplMap(SHFEORDRPLMAP& gmSHFEOrdRplMap)
{
	CWARN << "Printing gmSHFEOrdRplMap - printing all Rpl orders." << std::endl;
	SHFEORDERRPLMAPITER mShfeRplIter;

	for(mShfeRplIter = gmSHFEOrdRplMap.begin(); mShfeRplIter != gmSHFEOrdRplMap.end() ; mShfeRplIter++)
	{
		CWARN << "\n SHFE Rpl Order "
					<< "| To-be-replaced Order : ID=[" << mShfeRplIter->first
					<< "] -> To-Send Order : ID=[" << (mShfeRplIter->second).GetHandle()
					<< "] | Symbol=[" << (mShfeRplIter->second).GetSymbol()
					<< "] | Side =[" << (mShfeRplIter->second).GetSide()
					<< "]"
					<< std::endl;
	}
}

/// UTILITY FUNCTION: Prints sorted orders in multimap
void BidOfferSpread::PrintSortedOrdersMultimap(SORTED_ORDERS_MULTIMAP& multimap)
{
	CWARN << "Printing SortedOrdersMultimap - printing all orders." << std::endl;
	SORTED_ORDERS_MULTIMAP_ITER mOrderIter;

	for(mOrderIter = multimap.begin(); mOrderIter != multimap.end() ; ++mOrderIter)
	{
		CWARN << "\n StreetOrder | Price =[" <<  (*mOrderIter).first
				<< "] | OrderID =[" << ((*mOrderIter).second).GetHandle()
				<< "] | Symbol =[" <<  ((*mOrderIter).second).GetSymbol()
				<< "] | Side =[" << ((*mOrderIter).second).GetSide()
				<< "] | Qty =[" << ((*mOrderIter).second).GetSize()
				<< "]"
				<< std::endl;
	}
}

/// UTILITY FUNCTION: Prints sorted orders in multimap
bool BidOfferSpread::SymConnectionInit()
{
	if (!g_SymConnection)
	{
		CWARN << "SymConnection not initialized : Construct SymConnection!" << std::endl;
		g_SymConnection = new CSymConnection();
	}

	if(!g_SymConnection->Connect(getenv("FLEX_SYMHOST"), getenv("FLEX_SYMSERV"), 0))
	{
		CERR << "SymConnection : Not able to connect to Sym!" << std::endl;
		return false;
	}
	else
	{
		CINFO << "SymConnection : Connected to Sym!" << std::endl;
		return true;
	}
}
