// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2017-2019 The DragonBallChain Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.


#include "cointransfertx.h"

#include "main.h"

/**################################ Base Coin (WICC) Transfer ########################################**/
bool CBaseCoinTransferTx::CheckTx(CTxExecuteContext &context) {
    CValidationState &state = *context.pState;
    IMPLEMENT_CHECK_TX_MEMO;

    IMPLEMENT_CHECK_TX_REGID_OR_KEYID(toUid);

    if (coin_amount < DUST_AMOUNT_THRESHOLD)
        return state.DoS(100, ERRORMSG("dust amount, %llu < %llu", coin_amount,
                         DUST_AMOUNT_THRESHOLD), REJECT_DUST, "invalid-coin-amount");

    if ((txUid.is<CPubKey>()) && !txUid.get<CPubKey>().IsFullyValid())
        return state.DoS(100, ERRORMSG("public key is invalid"), REJECT_INVALID,
                         "bad-publickey");

    return true;
}

bool CBaseCoinTransferTx::ExecuteTx(CTxExecuteContext &context) {
    IMPLEMENT_DEFINE_CW_STATE;

    shared_ptr<CAccount> spDestAccount = GetAccount(*context.pCw, toUid);
    if (!spDestAccount) {
        if (toUid.is<CKeyID>()) { // first involved in transaction
            spDestAccount = NewAccount(cw, toUid.get<CKeyID>());
        } else {
            return state.DoS(100, ERRORMSG("%s, to account of transfer not exist, uid=%s",
                    TX_ERR_TITLE, toUid.ToString()),
                    READ_ACCOUNT_FAIL, "account-not-exist");
        }
    }
    if (!sp_tx_account->OperateBalance(SYMB::WICC, BalanceOpType::SUB_FREE, coin_amount,
                                       ReceiptType::TRANSFER_ACTUAL_COINS, receipts,
                                       spDestAccount.get())) {
        return state.DoS(100, ERRORMSG("%s, account has insufficient funds", TX_ERR_TITLE),
                        UPDATE_ACCOUNT_FAIL, "account-free-insufficient");
    }

    return true;
}

string CBaseCoinTransferTx::ToString(CAccountDBCache &accountCache) {
    return strprintf(
        "txType=%s, hash=%s, ver=%d, txUid=%s, toUid=%s, coin_amount=%llu, llFees=%llu, memo=%s, valid_height=%d",
        GetTxType(nTxType), GetHash().ToString(), nVersion, txUid.ToString(), toUid.ToString(), coin_amount, llFees,
        HexStr(memo), valid_height);
}

Object CBaseCoinTransferTx::ToJson(CCacheWrapper &cw) const {
    SingleTransfer transfer(toUid, SYMB::WICC, coin_amount);
    Array transferArray;
    transferArray.push_back(transfer.ToJson(cw));

    Object result = CBaseTx::ToJson(cw);
    result.push_back(Pair("transfers",   transferArray));
    result.push_back(Pair("memo",        memo));

    return result;
}

bool CCoinTransferTx::CheckMinFee(CTxExecuteContext &context, uint64_t minFee) {
    auto totalMinFee = transfers.size() * minFee;
    if (llFees < totalMinFee) {
        string err = strprintf("The given fee is too small: %llu < %llu sawi", llFees, totalMinFee);
        return context.pState->DoS(100, ERRORMSG("%s, tx=%s, height=%d, fee_symbol=%s",
            err, GetTxTypeName(), context.height, fee_symbol), REJECT_INVALID, err);
    }
    return true;
}

bool CCoinTransferTx::CheckTx(CTxExecuteContext &context) {
    IMPLEMENT_DEFINE_CW_STATE;
    IMPLEMENT_CHECK_TX_MEMO;

    if (transfers.empty() || transfers.size() > MAX_TRANSFER_SIZE) {
        return state.DoS(100, ERRORMSG("transfers is empty or too large count=%d than %d",
            transfers.size(), MAX_TRANSFER_SIZE), REJECT_INVALID, "invalid-transfers");
    }

    for (size_t i = 0; i < transfers.size(); i++) {
        if (transfers[i].to_uid.IsEmpty())
            return state.DoS(100, ERRORMSG("to_uid can not be empty"), REJECT_INVALID, "invalid-toUid");

        CAsset asset;
        if (!cw.assetCache.GetAsset(transfers[i].coin_symbol, asset))
            return state.DoS(100, ERRORMSG("transfers[%d], invalid coin_symbol=%s", i,
                            transfers[i].coin_symbol), REJECT_INVALID, "invalid-coin-symbol");

        if(!asset.HasPerms(AssetPermType::PERM_TRANSFER)) {
            return state.DoS(100, ERRORMSG("transfers[%d], lack perm, perm_name=PERM_TRANSFER, coin_symbol=%s", i,
                                           transfers[i].coin_symbol), REJECT_INVALID, "lack_PERM_TRANSFER");
        }

        if (transfers[i].coin_amount < DUST_AMOUNT_THRESHOLD)
            return state.DoS(100, ERRORMSG("transfers[%d], dust amount, %llu < %llu",
                i, transfers[i].coin_amount, DUST_AMOUNT_THRESHOLD), REJECT_DUST, "invalid-coin-amount");


        if (!CheckCoinRange(transfers[i].coin_symbol, transfers[i].coin_amount))
            return state.DoS(100,
                ERRORMSG("transfers[%d], coin_symbol=%s, coin_amount=%llu out of valid range",
                         i, transfers[i].coin_symbol, transfers[i].coin_amount), REJECT_DUST, "invalid-coin-amount");
    }

    if ((txUid.is<CPubKey>()) && !txUid.get<CPubKey>().IsFullyValid())
        return state.DoS(100, ERRORMSG("public key is invalid"), REJECT_INVALID,
                         "bad-publickey");

    return true;
}

bool CCoinTransferTx::ExecuteTx(CTxExecuteContext &context) {
    CCacheWrapper &cw       = *context.pCw;
    CValidationState &state = *context.pState;
    const auto &txid = GetHash();

    for (size_t i = 0; i < transfers.size(); i++) {
        const auto &transfer = transfers[i];
        const CUserID &toUid = transfer.to_uid;
        uint64_t actualCoinsToSend = transfer.coin_amount;
        // process WUSD transaction risk-reverse fees
        if (transfer.coin_symbol == SYMB::WUSD) {  // if transferring WUSD, must pay friction fees to the risk reserve
            uint64_t frictionFeeRatio;
            if (!cw.sysParamCache.GetParam(TRANSFER_SCOIN_FRICTION_FEE_RATIO, frictionFeeRatio))
                return state.DoS(100, ERRORMSG("transfers[%d], read TRANSFER_SCOIN_FRICTION_FEE_RATIO error", i),
                                READ_SYS_PARAM_FAIL, "bad-read-sysparamdb");
            uint64_t frictionFee = 0;
            if (!CalcAmountByRatio(transfer.coin_amount, frictionFeeRatio, RATIO_BOOST, frictionFee))
                return state.DoS(100, ERRORMSG("transfers[%d], the calc_friction_fee overflow! amount=%llu, "
                    "fee_ratio=%llu", i, transfer.coin_amount, frictionFeeRatio),
                    REJECT_INVALID, "calc-friction-fee-overflow");

            if (frictionFee > 0) {
                actualCoinsToSend -= frictionFee;

                auto spFcoinGenesisAccount = GetAccount(context, SysCfg().GetFcoinGenesisRegId(), "fcoin");
                if (!spFcoinGenesisAccount) return false;

                // 1) transfer all risk fee to risk-reserve
                if (!sp_tx_account->OperateBalance(SYMB::WUSD, BalanceOpType::SUB_FREE, frictionFee,
                                                ReceiptType::FRICTION_FEE, receipts,
                                                spFcoinGenesisAccount.get())) {
                    return state.DoS(100, ERRORMSG("transfer risk fee to risk-reserve account failed"),
                                    UPDATE_ACCOUNT_FAIL, "transfer-risk-fee-failed");
                }

                // 2) sell friction fees and burn it
                // should freeze user's coin for buying the WGRT
                if (!spFcoinGenesisAccount->OperateBalance(SYMB::WUSD, BalanceOpType::FREEZE, frictionFee,
                                                        ReceiptType::BUY_FCOINS_FOR_DEFLATION, receipts)) {
                    return state.DoS(100, ERRORMSG("account has insufficient funds"),
                                    UPDATE_ACCOUNT_FAIL, "operate-fcoin-genesis-account-failed");
                }
                CHashWriter hashWriter(SER_GETHASH, 0);
                hashWriter << txid << SYMB::WUSD << CFixedUInt32(i);
                uint256 orderId = hashWriter.GetHash();
                auto pSysBuyMarketOrder = dex::CSysOrder::CreateBuyMarketOrder(
                    context.GetTxCord(), SYMB::WUSD, SYMB::WGRT, frictionFee, {"send", txid});
                if (!cw.dexCache.CreateActiveOrder(orderId, *pSysBuyMarketOrder)) {
                    return state.DoS(100, ERRORMSG("create system buy order failed, orderId=%s", orderId.ToString()),
                                    CREATE_SYS_ORDER_FAILED, "create-sys-order-failed");
                }
            }
        }

        shared_ptr<CAccount> spDestAccount = GetAccount(*context.pCw, toUid);
        if (!spDestAccount) {
            if (toUid.is<CKeyID>()) {
                spDestAccount = NewAccount(cw, toUid.get<CKeyID>());
            } else if (toUid.is<CPubKey>()) {
                const auto& pubkey = toUid.get<CPubKey>();
                spDestAccount = NewAccount(cw, pubkey.GetKeyId());
            } else {
                return state.DoS(100, ERRORMSG("%s, to account of transfer not exist, uid=%s",
                        TX_ERR_TITLE, toUid.ToString()),
                        READ_ACCOUNT_FAIL, "account-not-exist");
            }
        }
        // register account, must have one dest only
        if (!txUid.is<CPubKey>() && transfers.size() == 1 && toUid.is<CPubKey>() && !spDestAccount->IsRegistered()) {
            if (!RegisterAccount(context, &toUid.get<CPubKey>(), *spDestAccount)) // generate new regid for the account
                return false;
        }

        if (!sp_tx_account->OperateBalance(transfer.coin_symbol, SUB_FREE, actualCoinsToSend,
                                           ReceiptType::TRANSFER_ACTUAL_COINS, receipts,
                                           spDestAccount.get())) {
            return state.DoS(100, ERRORMSG("%s, transfers[%d], transfer coin failed! fromUid=%s",
                    TX_ERR_TITLE, i, txUid.ToDebugString()), UPDATE_ACCOUNT_FAIL, "transfer-coin-failed");
        }
    }



    return true;
}

string CCoinTransferTx::ToString(CAccountDBCache &accountCache) {
    string transferStr = "";
    for (const auto &transfer : transfers) {
        if (!transferStr.empty()) transferStr += ",";
        transferStr += strprintf("{%s}", transfer.ToString(accountCache));
    }

    return strprintf(
        "txType=%s, hash=%s, ver=%d, txUid=%s, fee_symbol=%s, llFees=%llu, "
        "valid_height=%d, transfers=[%s], memo=%s",
        GetTxType(nTxType), GetHash().ToString(), nVersion, txUid.ToString(), fee_symbol, llFees,
        valid_height, transferStr, HexStr(memo));
}

Object CCoinTransferTx::ToJson(CCacheWrapper &cw) const {
    Object result = CBaseTx::ToJson(cw);

    Array transferArray;
    for (const auto &transfer : transfers) {
        transferArray.push_back(transfer.ToJson(cw));
    }

    result.push_back(Pair("transfers",   transferArray));
    result.push_back(Pair("memo",        memo));

    return result;
}
