// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2017-2019 The DragonBallChain Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockpricemediantx.h"
#include "main.h"

using namespace dex;

#define OBJ_COMPARE_LT1(obj1, obj2, f1) \
    ( obj1.f1 < obj2.f1 ? true : false )

#define OBJ_COMPARE_LT1_MORE(obj1, obj2, f1, more) \
     obj1.f1 < obj2.f1 ? true : ( obj2.f1 < obj1.f1 ? false : (more) )

#define OBJ_COMPARE_LT2(obj1, obj2, f1, f2) \
    OBJ_COMPARE_LT1_MORE( obj1, obj2, f1, OBJ_COMPARE_LT1(obj1, obj2, f2) )

#define OBJ_COMPARE_LT3(obj1, obj2, f1, f2, f3) \
    OBJ_COMPARE_LT1_MORE( obj1, obj2, f1, OBJ_COMPARE_LT2(obj1, obj2, f2, f3) )

#define OBJ_COMPARE_LT4(obj1, obj2, f1, f2, f3, f4) \
    OBJ_COMPARE_LT1_MORE( obj1, obj2, f1, OBJ_COMPARE_LT3(obj1, obj2, f2, f3, f4) )


static const CCdpCoinPair CDP_COIN_PAIR_WICC_WUSD = {SYMB::WICC, SYMB::WUSD};
static const CCdpCoinPair CDP_COIN_PAIR_WGRT_WUSD = {SYMB::WGRT, SYMB::WUSD};

bool operator<(const CCdpCoinPairDetail &a, const CCdpCoinPairDetail &b) {
    return OBJ_COMPARE_LT4(a, b, is_price_active, is_staked_perm, init_tx_cord, coin_pair);
}

bool GetCdpCoinPairDetails(CCacheWrapper &cw, HeightType height, const PriceDetailMap &priceDetails, set<CCdpCoinPairDetail> &cdpCoinPairSet) {
     uint64_t priceTimeoutBlocks = 0;
    if (!cw.sysParamCache.GetParam(SysParamType::PRICE_FEED_TIMEOUT_BLOCKS, priceTimeoutBlocks)) {
        return ERRORMSG("read sys param PRICE_FEED_TIMEOUT_BLOCKS error");
    }
    FeatureForkVersionEnum version = GetFeatureForkVersion(height);
    for (const auto& item : priceDetails) {
        if (item.first == kFcoinPriceCoinPair) continue;

        CAsset asset;
        const TokenSymbol &bcoinSymbol = item.first.first;
        const TokenSymbol &quoteSymbol = item.first.second;

        TokenSymbol scoinSymbol = GetCdpScoinByQuoteSymbol(quoteSymbol);
        if (scoinSymbol.empty()) {
            LogPrint(BCLog::CDP, "quote_symbol=%s not have a corresponding scoin , ignore", bcoinSymbol);
            continue;
        }

        // TODO: remove me if need to support multi scoin and improve the force liquidate process
        if (scoinSymbol != SYMB::WUSD)
            throw runtime_error(strprintf("only support to force liquidate scoin=WUSD, actual_scoin=%s", scoinSymbol));

        CCdpBcoinDetail cdpBcoinDetail;
        if (!cw.cdpCache.GetCdpBcoin(bcoinSymbol, cdpBcoinDetail)) {
            LogPrint(BCLog::CDP, "asset=%s not be activated as bcoin, ignore", bcoinSymbol);
            continue;
        }

        bool isPriceActive = true;
        if (version >= MAJOR_VER_R3 && !item.second.IsActive(height, priceTimeoutBlocks)) {
            isPriceActive = false;
        }

        cdpCoinPairSet.insert({
            CCdpCoinPair(bcoinSymbol, scoinSymbol), // coin_pair
            isPriceActive,                          // is_price_active
            true,                                   // is_staked_perm
            item.second.price,                      // bcoin_price
            cdpBcoinDetail.init_tx_cord             // init_tx_cord
        });
    }
    return true;
}

class CCdpForceLiquidator {
public:
    uint32_t liquidated_count = 0;
public:
    CCdpForceLiquidator(CBlockPriceMedianTx &txIn, CTxExecuteContext &contextIn,
                        vector<CReceipt> &receiptsIn, CAccount &fcoinAccountIn,
                        const CCdpCoinPairDetail &cdpCdoinPairDetailIn, uint64_t fcoindUsdPriceIn,
                        uint32_t &liquidatedLimitCountIn)
        : tx(txIn), context(contextIn), receipts(receiptsIn),
          fcoinAccount(fcoinAccountIn), cdp_cdoin_pair_detail(cdpCdoinPairDetailIn),
          fcoin_usd_price(fcoindUsdPriceIn),
          liquidated_limit_count(liquidatedLimitCountIn) {}

    bool Execute();

private:
    // input params
    CBlockPriceMedianTx &tx;
    CTxExecuteContext &context;
    vector<CReceipt> &receipts;
    CAccount &fcoinAccount;
    const CCdpCoinPairDetail &cdp_cdoin_pair_detail;
    uint64_t fcoin_usd_price = 0;
    uint32_t liquidated_limit_count = 0;

    bool SellAssetToRiskRevervePool(const CUserCDP &cdp, const TokenSymbol &assetSymbol,
                                    uint64_t amount, const TokenSymbol &coinSymbol,
                                    const uint256 &orderId, shared_ptr<CDEXOrderDetail> &pOrderOut,
                                    ReceiptType code, ReceiptList &receipts);

    uint256 GenOrderId(const CUserCDP &cdp, TokenSymbol assetSymbol);


    bool ForceLiquidateCDPCompat(const list<CUserCDP> &cdpList, ReceiptList &receipts);

    uint256 GenOrderIdCompat(const uint256 &txid, uint32_t index);
};

////////////////////////////////////////////////////////////////////////////////
// class CBlockPriceMedianTx
bool CBlockPriceMedianTx::CheckTx(CTxExecuteContext &context) { return true; }

/**
 *  force settle/liquidate any under-collateralized CDP (collateral ratio <= 104%)
 */
bool CBlockPriceMedianTx::ExecuteTx(CTxExecuteContext &context) {
    IMPLEMENT_DEFINE_CW_STATE;

    PriceDetailMap priceDetails;
    if (!cw.ppCache.CalcMedianPriceDetails(cw, context.height, priceDetails))
        return state.DoS(100, ERRORMSG("calc block median price points failed"),
                         READ_PRICE_POINT_FAIL, "calc-median-prices-failed");

    if (!EqualToCalculatedPrices(priceDetails)) {
        string str;
        for (const auto &item : priceDetails) {
            str += strprintf("{coin_pair=%s, price:%llu},", CoinPairToString(item.first), item.second.price);
        }

        LogPrint(BCLog::ERROR, "calc from cache, height=%d, price map={%s}\n",
                context.height, str);

        str.clear();
        for (const auto &item : median_prices) {
            str += strprintf("{coin_pair=%s, price=%llu}", CoinPairToString(item.first), item.second);
        }

        LogPrint(BCLog::ERROR, "from median tx, height: %d, price map: %s\n",
                context.height, str);

        return state.DoS(100, ERRORMSG("invalid median price points"), REJECT_INVALID,
                         "bad-median-price-points");
    }

    if (!cw.priceFeedCache.SetMedianPrices(priceDetails))
        return state.DoS(100, ERRORMSG("save median prices to db failed"), REJECT_INVALID,
                         "save-median-prices-failed");

    if (!ForceLiquidateCdps(context, priceDetails))
        return false; // error msg has been processed

    return true;
}

bool CBlockPriceMedianTx::ForceLiquidateCdps(CTxExecuteContext &context, PriceDetailMap &priceDetails) {
    CCacheWrapper &cw = *context.pCw;  CValidationState &state = *context.pState;

    FeatureForkVersionEnum version = GetFeatureForkVersion(context.height);
    auto fcoinIt = priceDetails.find(kFcoinPriceCoinPair);
    if (fcoinIt == priceDetails.end() || fcoinIt->second.price == 0) {
        LogPrint(BCLog::CDP, "price of fcoin(%s) is 0, ignore\n", CoinPairToString(kFcoinPriceCoinPair));
        return true;

    }
    uint64_t priceTimeoutBlocks = 0;
    if (!cw.sysParamCache.GetParam(SysParamType::PRICE_FEED_TIMEOUT_BLOCKS, priceTimeoutBlocks)) {
        return state.DoS(100, ERRORMSG("read sys param PRICE_FEED_TIMEOUT_BLOCKS error"),
                REJECT_INVALID, "read-sysparam-error");
    }
    if (!fcoinIt->second.IsActive(context.height, priceTimeoutBlocks)) {
        LogPrint(BCLog::CDP,
                 "price of fcoin(%s) is inactive, ignore, "
                 "last_update_height=%u, cur_height=%u\n",
                 CoinPairToString(kFcoinPriceCoinPair), fcoinIt->second.last_feed_height,
                 context.height);
        return true;
    }

    uint64_t fcoinUsdPrice = fcoinIt->second.price;

    auto spFcoinAccount = GetAccount(context, SysCfg().GetFcoinGenesisRegId(), "fcoin");
    if (!spFcoinAccount) return false;

    set<CCdpCoinPairDetail> cdpCoinPairSet;

    if (!GetCdpCoinPairDetails(cw, context.height, priceDetails, cdpCoinPairSet)) {
        return state.DoS(100, ERRORMSG("get cdp coin pairs error"),
                REJECT_INVALID, "get-cdp-coin-pairs-error");
    }

    uint32_t liquidatedLimitCount = CDP_FORCE_LIQUIDATE_MAX_COUNT;
    for (const auto& cdpCoinPairDetail : cdpCoinPairSet) {

        if (version >= MAJOR_VER_R3 && !cdpCoinPairDetail.is_price_active) {
            LogPrint(BCLog::CDP,
                    "price of coin_pair(%s) is inactive, ignore\n",
                    cdpCoinPairDetail.coin_pair.ToString());
            continue;
        }

        CCdpForceLiquidator forceLiquidator(*this, context, receipts, *spFcoinAccount,
                                            cdpCoinPairDetail, fcoinUsdPrice, liquidatedLimitCount);
        if (!forceLiquidator.Execute())
            return false; // the forceLiquidator.Execute() has processed the error
        if (forceLiquidator.liquidated_count >= liquidatedLimitCount) {
            break;
        }
        liquidatedLimitCount -= forceLiquidator.liquidated_count;
    }

    return true;
}

bool CBlockPriceMedianTx::EqualToCalculatedPrices(const PriceDetailMap &calcPrices) {

    PriceMap medianPrices;
    for (auto &item : median_prices) {
        if (item.second != 0)
            medianPrices.insert(item);
    }
    // the calcPrices must not contain 0 price item
    if (medianPrices.size() != calcPrices.size()) return false;

    auto priceIt = medianPrices.begin();
    auto detailIt = calcPrices.begin();
    for(; priceIt != medianPrices.end() && detailIt != calcPrices.end(); priceIt++, detailIt++) {
        if (priceIt->first != detailIt->first || priceIt->second != detailIt->second.price)
            return false;
    }
    return priceIt == medianPrices.end() && detailIt == calcPrices.end();
}

string CBlockPriceMedianTx::ToString(CAccountDBCache &accountCache) {
    string pricePoints;
    for (const auto item : median_prices) {
        pricePoints += strprintf("{coin_symbol:%s, price_symbol:%s, price:%lld}", item.first.first, item.first.second,
                                 item.second);
    }

    return strprintf("txType=%s, hash=%s, ver=%d, txUid=%s, llFees=%ld, median_prices=%s, valid_height=%d",
                     GetTxType(nTxType), GetHash().GetHex(), nVersion, txUid.ToString(), llFees, pricePoints,
                     valid_height);
}

Object CBlockPriceMedianTx::ToJson(CCacheWrapper &cw) const {
    Object result = CBaseTx::ToJson(cw);

    Array pricePointArray;
    for (const auto &item : median_prices) {
        Object subItem;
        subItem.push_back(Pair("coin_symbol",     item.first.first));
        subItem.push_back(Pair("price_symbol",    item.first.second));
        subItem.push_back(Pair("price",           item.second));
        pricePointArray.push_back(subItem);
    }
    result.push_back(Pair("median_price_points",   pricePointArray));

    return result;
}

///////////////////////////////////////////////////////////////////////////////
// class CCdpForceLiquidator


bool CCdpForceLiquidator::Execute() {

    CCacheWrapper &cw = *context.pCw; CValidationState &state = *context.pState;

    const CCdpCoinPair &cdpCoinPair = cdp_cdoin_pair_detail.coin_pair;
    uint64_t bcoinPrice = cdp_cdoin_pair_detail.bcoin_price;
    // 1. Check Global Collateral Ratio floor & Collateral Ceiling if reached
    uint64_t globalCollateralRatioFloor = 0;

    if (!cw.sysParamCache.GetCdpParam(cdpCoinPair, CdpParamType::CDP_GLOBAL_COLLATERAL_RATIO_MIN, globalCollateralRatioFloor)) {
        return state.DoS(100, ERRORMSG("read global collateral ratio floor param error! cdpCoinPair=%s",
                cdpCoinPair.ToString()),
                READ_SYS_PARAM_FAIL, "read-global-collateral-ratio-floor-error");
    }

    CCdpGlobalData cdpGlobalData = cw.cdpCache.GetCdpGlobalData(cdpCoinPair);
    // check global collateral ratio
    if (cdpGlobalData.CheckGlobalCollateralRatioFloorReached(bcoinPrice, globalCollateralRatioFloor)) {
        LogPrint(BCLog::CDP, "GlobalCollateralFloorReached!!\n");
        return true;
    }

    // 2. get all CDPs to be force settled
    uint64_t forceLiquidateRatio = 0;
    if (!cw.sysParamCache.GetCdpParam(cdpCoinPair, CdpParamType::CDP_FORCE_LIQUIDATE_RATIO, forceLiquidateRatio)) {
        return state.DoS(100, ERRORMSG("read force liquidate ratio param error! cdpCoinPair=%s",
                cdpCoinPair.ToString()),
                READ_SYS_PARAM_FAIL, "read-force-liquidate-ratio-error");
    }

    const auto &cdpList = cw.cdpCache.GetCdpListByCollateralRatio(cdpCoinPair, forceLiquidateRatio, bcoinPrice);

    LogPrint(BCLog::CDP, "[%d] globalCollateralRatioFloor=%llu, bcoin_price: %llu, "
            "forceLiquidateRatio: %llu, cdp_count: %llu\n", context.height,
            globalCollateralRatioFloor, bcoinPrice, forceLiquidateRatio, cdpList.size());

    // 3. force settle each cdp
    if (cdpList.size() == 0) return true;

    {
        // TODO: remove me.
        // print all force liquidating cdps
        LogPrint(BCLog::CDP, "have %llu cdps to force settle, in detail:\n", cdpList.size());
        for (const auto &cdp : cdpList) {
            LogPrint(BCLog::CDP, "%s\n", cdp.ToString());
        }
    }

    NET_TYPE netType = SysCfg().NetworkID();
    if (netType == TEST_NET && context.height < 1800000  && cdpCoinPair == CDP_COIN_PAIR_WICC_WUSD) {
        // soft fork to compat old data of testnet
        // TODO: remove me if reset testnet.
        return ForceLiquidateCDPCompat(cdpList, receipts);
    }

    for (const auto &cdp : cdpList) {
        liquidated_count++;
        if (liquidated_count > liquidated_limit_count) {
            LogPrint(BCLog::CDP, "force liquidate cdp count=%u reach the max liquidated limit count=%u! cdp_coin_pair={%s}\n",
                    liquidated_count, liquidated_limit_count, cdpCoinPair.ToString());
            break;
        }

        uint64_t currRiskReserveScoins = fcoinAccount.GetToken(SYMB::WUSD).free_amount;
        if (currRiskReserveScoins < cdp.total_owed_scoins) {
            LogPrint(BCLog::CDP, "currRiskReserveScoins(%lu) < cdp.total_owed_scoins(%lu) !!\n",
                    currRiskReserveScoins, cdp.total_owed_scoins);
            break;
        }

        auto spCdpOwnerAccount = tx.GetAccount(context, cdp.owner_regid, "cdp_owner");
        if (!spCdpOwnerAccount) return false;

        LogPrint(BCLog::CDP,
                    "begin to force settle CDP {%s}, currRiskReserveScoins: %llu, "
                    "index: %u\n", cdp.ToString(), currRiskReserveScoins, liquidated_count - 1);

        // a) get scoins from risk reserve pool for closeout
        ReceiptType code = ReceiptType::CDP_TOTAL_CLOSEOUT_SCOIN_FROM_RESERVE;
        fcoinAccount.OperateBalance(SYMB::WUSD, BalanceOpType::SUB_FREE, cdp.total_owed_scoins, code, receipts);

        // b) sell bcoins for risk reserve pool
        // b.1) clean up cdp owner's pledged_amount
        auto assetReceiptCode = ReceiptType::CDP_TOTAL_ASSET_TO_RESERVE;
        if (!spCdpOwnerAccount->OperateBalance(cdp.bcoin_symbol, UNPLEDGE, cdp.total_staked_bcoins,
                                               assetReceiptCode, receipts)) {
            return state.DoS(100, ERRORMSG("unpledge bcoins failed! cdp={%s}", cdp.ToString()),
                    UPDATE_ACCOUNT_FAIL, "unpledge-bcoins-failed");
        }

        if (!spCdpOwnerAccount->OperateBalance(cdp.bcoin_symbol, SUB_FREE, cdp.total_staked_bcoins,
                                               assetReceiptCode, receipts, &fcoinAccount)) {
            return state.DoS(100, ERRORMSG("sub unpledged bcoins failed! cdp={%s}", cdp.ToString()),
                    UPDATE_ACCOUNT_FAIL, "deduct-bcoins-failed");
        }

        // b.2) sell bcoins to get scoins and put them to risk reserve pool
        uint256 assetSellOrderId = GenOrderId(cdp, cdpCoinPair.bcoin_symbol);
        shared_ptr<CDEXOrderDetail> pAssetSellOrder;
        if (!SellAssetToRiskRevervePool(cdp, cdpCoinPair.bcoin_symbol, cdp.total_staked_bcoins,
                                        cdpCoinPair.scoin_symbol, assetSellOrderId, pAssetSellOrder,
                                        assetReceiptCode, receipts))
            return false;

        // c) inflate WGRT coins to risk reserve pool and sell them to get WUSD  if necessary
        uint64_t bcoinsValueInScoin = uint64_t(double(cdp.total_staked_bcoins) * bcoinPrice / PRICE_BOOST);
        if (bcoinsValueInScoin < cdp.total_owed_scoins) {  // 0 ~ 1
            uint64_t fcoinsValueToInflate = cdp.total_owed_scoins - bcoinsValueInScoin;
            assert(fcoin_usd_price != 0);
            uint64_t fcoinsToInflate = uint64_t(double(fcoinsValueToInflate) * PRICE_BOOST / fcoin_usd_price);
            ReceiptType inflateFcoinCode = ReceiptType::CDP_TOTAL_INFLATE_FCOIN_TO_RESERVE;

            // inflate fcoin to fcoin genesis account
            if (!fcoinAccount.OperateBalance(SYMB::WGRT, BalanceOpType::ADD_FREE, fcoinsToInflate, inflateFcoinCode, receipts)) {
                return context.pState->DoS(100, ERRORMSG("add account balance failed"),
                                    UPDATE_ACCOUNT_FAIL, "operate-fcoin-genesis-account-failed");
            }

            uint256 fcoinSellOrderId = GenOrderId(cdp, SYMB::WGRT);
            shared_ptr<CDEXOrderDetail> pFcoinSellOrder;
            if (!SellAssetToRiskRevervePool(cdp, SYMB::WGRT, fcoinsToInflate,
                                            cdpCoinPair.scoin_symbol, fcoinSellOrderId,
                                            pFcoinSellOrder, inflateFcoinCode, receipts)) {
                return false;
            }

            LogPrint(BCLog::CDP, "Force settled CDP: "
                "Placed BcoinSellMarketOrder:  %s, orderId: %s\n"
                "Placed FcoinSellMarketOrder:  %s, orderId: %s\n"
                "prevRiskReserveScoins: %lu -> currRiskReserveScoins: %lu\n",
                pAssetSellOrder->ToString(), assetSellOrderId.GetHex(),
                pFcoinSellOrder->ToString(), fcoinSellOrderId.GetHex(),
                currRiskReserveScoins, currRiskReserveScoins - cdp.total_owed_scoins);
        } else  {  // 1 ~ 1.04
            // The sold assets are sufficient to pay off the debt
            LogPrint(BCLog::CDP, "Force settled CDP: "
                "Placed BcoinSellMarketOrder: %s, orderId: %s\n"
                "No need to infate WGRT coins: %llu vs %llu\n"
                "prevRiskReserveScoins: %lu -> currRiskReserveScoins: %lu\n",
                pAssetSellOrder->ToString(), assetSellOrderId.GetHex(),
                bcoinsValueInScoin, cdp.total_owed_scoins,
                currRiskReserveScoins, currRiskReserveScoins - cdp.total_owed_scoins);
        }

        // c) Close the CDP
        const CUserCDP &oldCDP = cdp;
        cw.cdpCache.EraseCDP(oldCDP, cdp);
        if (SysCfg().GetArg("-persistclosedcdp", false)) {
            if (!cw.closedCdpCache.AddClosedCdpIndex(oldCDP.cdpid, tx.GetHash(), CDPCloseType::BY_FORCE_LIQUIDATE)) {
                LogPrint(BCLog::ERROR, "persistclosedcdp add failed for force-liquidated cdpid (%s)", oldCDP.cdpid.GetHex());
            }
        }
    }

    return true;
}

bool CCdpForceLiquidator::SellAssetToRiskRevervePool(const CUserCDP &cdp,
                                                     const TokenSymbol &assetSymbol,
                                                     uint64_t amount, const TokenSymbol &coinSymbol,
                                                     const uint256 &orderId,
                                                     shared_ptr<CDEXOrderDetail> &pOrderOut,
                                                     ReceiptType code, ReceiptList &receipts) {

    // freeze account asset for selling
    if (!fcoinAccount.OperateBalance(assetSymbol, BalanceOpType::FREEZE, amount, code, receipts)) {
        return context.pState->DoS(100, ERRORMSG("account has insufficient funds"),
                            UPDATE_ACCOUNT_FAIL, "account-insufficient");
    }

    pOrderOut = dex::CSysOrder::CreateSellMarketOrder(
        CTxCord(context.height, context.index), coinSymbol, assetSymbol, amount, {"cdp_asset", cdp.cdpid});

    if (!context.pCw->dexCache.CreateActiveOrder(orderId, *pOrderOut)) {
        return context.pState->DoS(100, ERRORMSG("create sys sell market order failed, cdpid=%s, "
                "assetSymbol=%s, coinSymbol=%s, amount=%llu",
                cdp.cdpid.ToString(), assetSymbol, coinSymbol, amount),
                CREATE_SYS_ORDER_FAILED, "create-sys-order-failed");
    }
    LogPrint(BCLog::DEX, "create sys sell market order OK! cdpid=%s, order_detail={%s}",
                cdp.cdpid.ToString(), pOrderOut->ToString());

    return true;
}

uint256 CCdpForceLiquidator::GenOrderId(const CUserCDP &cdp, TokenSymbol assetSymbol) {

    CHashWriter ss(SER_GETHASH, 0);
    ss << cdp.cdpid << assetSymbol;
    return ss.GetHash();
}


bool CCdpForceLiquidator::ForceLiquidateCDPCompat(const list<CUserCDP> &cdpList, ReceiptList &receipts) {

    CCacheWrapper &cw = *context.pCw; CValidationState &state = *context.pState;
    const uint256 &txid = tx.GetHash();
    uint64_t bcoinPrice = cdp_cdoin_pair_detail.bcoin_price;
    // sort by CUserCDP::operator<()
    set<CUserCDP> cdpSet;
    for (auto &cdp : cdpList) {
        cdpSet.insert(cdp);
    }

    uint64_t currRiskReserveScoins = fcoinAccount.GetToken(SYMB::WUSD).free_amount;
    uint32_t orderIndex            = 0;
    for (auto &cdp : cdpSet) {
        const auto &cdpCoinPair = cdp.GetCoinPair();
        liquidated_count++;
        if (liquidated_count > liquidated_limit_count) {
            LogPrint(BCLog::CDP, "force liquidate cdp count=%u reach the max liquidated limit count=%u! cdp_coin_pair={%s}\n",
                    liquidated_count, liquidated_limit_count, cdpCoinPair.ToString());
            break;
        }
        LogPrint(BCLog::CDP,
                    "begin to force settle CDP (%s), currRiskReserveScoins: %llu, "
                    "index: %u\n", cdp.ToString(), currRiskReserveScoins, liquidated_count - 1);

        if (currRiskReserveScoins < cdp.total_owed_scoins) {
            LogPrint(BCLog::CDP, "currRiskReserveScoins(%lu) < cdp.total_owed_scoins(%lu) !!\n",
                    currRiskReserveScoins, cdp.total_owed_scoins);
            continue;
        }

        auto spCdpOwnerAccount = tx.GetAccount(cw, cdp.owner_regid);
        if (!spCdpOwnerAccount) return false;

        auto assetReceiptCode = ReceiptType::CDP_TOTAL_ASSET_TO_RESERVE;
        if (!spCdpOwnerAccount->OperateBalance(cdp.bcoin_symbol, UNPLEDGE, cdp.total_staked_bcoins,
                                               assetReceiptCode, receipts)) {
            return state.DoS(100, ERRORMSG("unpledge bcoins failed! cdp={%s}", cdp.ToString()),
                    UPDATE_ACCOUNT_FAIL, "unpledge-bcoins-failed");
        }

        if (!spCdpOwnerAccount->OperateBalance(cdp.bcoin_symbol, SUB_FREE, cdp.total_staked_bcoins,
                                               assetReceiptCode, receipts,
                                               &fcoinAccount)) {
            return state.DoS(100, ERRORMSG("transfer forced-liquidate assets to risk reserve failed! cdp={%s}",
                    cdp.ToString()), UPDATE_ACCOUNT_FAIL, "transfer-forced-liquidate-assets-failed");
        }

        uint256 bcoinSellMarketOrderId = GenOrderIdCompat(txid, orderIndex++);
        shared_ptr<CDEXOrderDetail> pBcoinSellMarketOrder;
        if (!SellAssetToRiskRevervePool(cdp, SYMB::WICC, cdp.total_staked_bcoins, SYMB::WUSD,
                                        bcoinSellMarketOrderId, pBcoinSellMarketOrder,
                                        assetReceiptCode, receipts)) {
            return false;
        }
        // b) inflate WGRT coins and sell them for WUSD to return to risk reserve pool if necessary
        uint64_t bcoinsValueInScoin = uint64_t(double(cdp.total_staked_bcoins) * bcoinPrice / PRICE_BOOST);
        if (bcoinsValueInScoin >= cdp.total_owed_scoins) {  // 1 ~ 1.04
            LogPrint(BCLog::CDP, "Force settled CDP: "
                "Placed BcoinSellMarketOrder: %s, orderId: %s\n"
                "No need to infate WGRT coins: %llu vs %llu\n"
                "prevRiskReserveScoins: %lu -> currRiskReserveScoins: %lu\n",
                pBcoinSellMarketOrder->ToString(), bcoinSellMarketOrderId.GetHex(),
                bcoinsValueInScoin, cdp.total_owed_scoins,
                currRiskReserveScoins, currRiskReserveScoins - cdp.total_owed_scoins);
        } else {  // 0 ~ 1
            uint64_t fcoinsValueToInflate = cdp.total_owed_scoins - bcoinsValueInScoin;
            assert(fcoin_usd_price != 0);
            uint64_t fcoinsToInflate = uint64_t(double(fcoinsValueToInflate) * PRICE_BOOST / fcoin_usd_price);
            ReceiptType inflateFcoinCode = ReceiptType::CDP_TOTAL_INFLATE_FCOIN_TO_RESERVE;
            // inflate fcoin to fcoin genesis account
            if (!fcoinAccount.OperateBalance(SYMB::WGRT, BalanceOpType::ADD_FREE, fcoinsToInflate,
                                                    inflateFcoinCode, receipts)) {
                return state.DoS(100, ERRORMSG("operate balance failed"),
                                    UPDATE_ACCOUNT_FAIL, "operate-fcoin-genesis-account-failed");
            }

            uint256 fcoinSellMarketOrderId = GenOrderIdCompat(txid, orderIndex++);
            shared_ptr<CDEXOrderDetail> pFcoinSellOrder;
            if (!SellAssetToRiskRevervePool(cdp, SYMB::WGRT, fcoinsToInflate, SYMB::WUSD,
                                            fcoinSellMarketOrderId, pFcoinSellOrder,
                                            inflateFcoinCode, receipts)) {
                return false;
            }

            LogPrint(BCLog::CDP, "Force settled CDP: "
                "Placed BcoinSellOrder:  %s, orderId: %s\n"
                "Placed FcoinSellOrder:  %s, orderId: %s\n"
                "prevRiskReserveScoins: %lu -> currRiskReserveScoins: %lu\n",
                pBcoinSellMarketOrder->ToString(), bcoinSellMarketOrderId.GetHex(),
                pFcoinSellOrder->ToString(), fcoinSellMarketOrderId.GetHex(),
                currRiskReserveScoins, currRiskReserveScoins - cdp.total_owed_scoins);
        }

        // c) Close the CDP
        const CUserCDP &oldCDP = cdp;
        cw.cdpCache.EraseCDP(oldCDP, cdp);
        if (SysCfg().GetArg("-persistclosedcdp", false)) {
            if (!cw.closedCdpCache.AddClosedCdpIndex(oldCDP.cdpid, tx.GetHash(), CDPCloseType::BY_FORCE_LIQUIDATE)) {
                LogPrint(BCLog::ERROR, "persistclosedcdp add failed for force-liquidated cdpid (%s)", oldCDP.cdpid.GetHex());
            }
        }

        // d) minus scoins from the risk reserve pool to repay CDP scoins
        currRiskReserveScoins -= cdp.total_owed_scoins;
    }

    // 4. operate fcoin genesis account
    uint64_t prevScoins = fcoinAccount.GetToken(SYMB::WUSD).free_amount;
    assert(prevScoins >= currRiskReserveScoins);

    if (!fcoinAccount.OperateBalance(SYMB::WUSD, SUB_FREE, prevScoins - currRiskReserveScoins,
                                            ReceiptType::CDP_TOTAL_INFLATE_FCOIN_TO_RESERVE, receipts)) {
        return state.DoS(100, ERRORMSG("opeate fcoin genesis account failed"),
                            UPDATE_ACCOUNT_FAIL, "operate-fcoin-genesis-account-failed");
    }

    return true;
}


// gen orderid compat with testnet old data
// Generally, index is an auto increase variable.
// TODO: remove me if reset testnet.
uint256 CCdpForceLiquidator::GenOrderIdCompat(const uint256 &txid, uint32_t index) {

    CHashWriter ss(SER_GETHASH, 0);
    ss << txid << VARINT(index);
    return ss.GetHash();
}
